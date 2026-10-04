import json
import os
from pathlib import Path
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from sdk_wheel import prepare_sdk


def main():
    build_dir, wheel_dir, bindir = sys.argv[1:]
    build_dir, wheel_dir = Path(build_dir).resolve(), Path(wheel_dir).resolve()
    cmake = shutil.which('cmake')
    assert cmake, 'FAIL cmake missing'
    assert not Path(bindir).is_absolute(), 'FAIL test prefix requires a relative install bindir'
    with tempfile.TemporaryDirectory(prefix='chronolog-install-') as scratch:
        root = Path(scratch)
        sdk = prepare_sdk(root / 'sdk-wheels', wheel_dir)
        prefix = root / 'prefix'
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('CHRONOLOG_', 'PYTHON', 'LD_'))}
        env['PATH'] = '/usr/bin:/bin'
        env['CHRONOLOG_HOME'] = str(root / 'state')

        def call(command, timeout=60):
            result = subprocess.run([str(arg) for arg in command], cwd=root, env=env,
                                    text=True, capture_output=True, timeout=timeout)
            if result.returncode:
                raise RuntimeError('FAIL ' + str(command) + '\n' + result.stdout + result.stderr)
            return result.stdout

        call([sys.executable, '-m', 'venv', prefix])
        servers = {'visor': 'src/visor', 'keeper': 'src/keeper',
                   'grapher': 'src/grapher', 'player': 'src/player'}
        before = {role: (build_dir / folder / ('chrono_' + role)).stat().st_size
                  for role, folder in servers.items()}
        started = time.monotonic()
        for component in ('server', 'client'):
            command = [cmake, '--install', build_dir, '--prefix', prefix, '--component', component]
            if component == 'server':
                command.append('--strip')
            call(command)
            if component == 'server':
                elapsed = time.monotonic() - started
                after = {role: (prefix / bindir / ('chrono_' + role)).stat().st_size
                         for role in servers}
                for role in servers:
                    print(f'Installed chrono_{role} bytes: before={before[role]} after={after[role]}',
                          flush=True)
                print(f'Installed servers total bytes: before={sum(before.values())} '
                      f'after={sum(after.values())}; cmake --install --strip seconds={elapsed:.3f}',
                      flush=True)
        python = prefix / 'bin/python'
        wheels = [next(wheel_dir.glob('chronolog_local-*.whl')), sdk]
        call([python, '-m', 'pip', 'install', '--no-index', '--no-deps', *wheels])
        cli = prefix / 'bin/chronolog'
        # Only the installed prefix and system tools are visible; the binding comes from this venv.
        env['PATH'] = str(prefix / bindir) + ':/usr/bin:/bin'
        doctor = json.loads(call([cli, 'doctor']))
        for role in ('visor', 'keeper', 'grapher', 'player'):
            expected = prefix / bindir / ('chrono_' + role)
            assert Path(doctor['binaries'][role]) == expected, 'FAIL installed binary resolution ' + role
        base = None
        for _ in range(3):
            candidate = 10000 + random.randrange(4399) * 5
            sockets = []
            try:
                for offset in range(10):
                    sock = socket.socket()
                    sockets.append(sock)
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    sock.bind(('127.0.0.1', candidate + offset))
                base = candidate
                break
            except OSError:
                pass
            finally:
                for sock in sockets:
                    sock.close()
        assert base is not None, 'FAIL two five-port test blocks unavailable'
        ready = None
        try:
            ready = json.loads(call([cli, 'up', '--ephemeral', '--port-base', str(base)]))
            assert ready['state'] == 'ready' and ready['probe'] == 'rpc', 'FAIL installed RPC readiness'
            for service in ready['services'].values():
                exe = (Path('/proc') / str(service['pid']) / 'exe').resolve()
                assert exe.is_relative_to(prefix), 'FAIL service must run from installed prefix'
            client = Path(__file__).with_name('installed_client.py')
            # Stage the small client under the prefix so the process has no source-tree import path.
            staged = prefix / 'installed_client.py'
            shutil.copyfile(client, staged)
            print(call([python, '-I', staged, prefix, ready['endpoints']['catalog'],
                        ready['endpoints']['player']]), end='')
        finally:
            if ready:
                stopped = json.loads(call([cli, 'down', '--purge'], timeout=210))
                assert stopped['state'] == 'stopped', 'FAIL ordered stop'
                assert not (root / 'state/instances/default').exists(), 'FAIL purge'
        print('PASS launcher.server_install')


if __name__ == '__main__':
    main()
