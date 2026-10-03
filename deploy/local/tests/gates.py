import json
import os
from pathlib import Path
import random
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chronolog_local.registry import free_ports


def require(condition, message):
    if not condition:
        raise AssertionError('FAIL ' + message)


def until(predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError('FAIL bounded state wait')


def dead(pid):
    path = Path('/proc') / str(pid) / 'stat'
    return not path.exists() or path.read_text().rsplit(')', 1)[1].split()[0] == 'Z'


def main():
    gate, cli, bin_dir, driver = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='chronolog-local-') as temp:
        root = Path(temp)
        env = dict(os.environ, CHRONOLOG_HOME=str(root / 'registry'), CHRONOLOG_BIN_DIR=bin_dir)
        env.pop('CHRONOLOG_CATALOG', None)
        env.pop('CHRONOLOG_INSTANCE', None)
        base = None
        # Two adjacent five-port blocks, with all ten ports reserved during selection.
        for _ in range(3):
            candidate = 10000 + random.randrange(4399) * 5
            try:
                free_ports({str(i): f'127.0.0.1:{candidate + i}' for i in range(10)})
                base = candidate
                break
            except ValueError:
                pass
        require(base is not None, 'port allocation')

        def call(*args, ok=True, timeout=210):
            result = subprocess.run([sys.executable, cli, *args], env=env, text=True,
                                    capture_output=True, timeout=timeout)
            if ok:
                require(result.returncode == 0, result.stderr)
                return json.loads(result.stdout)
            require(result.returncode != 0, 'command should fail: ' + str(args))
            return result.stderr

        def helper(mode, record):
            result = subprocess.run([driver, mode, record['endpoints']['catalog'],
                                     record['endpoints']['player'], str(root / 'events')],
                                    capture_output=True, text=True, timeout=60)
            require(result.returncode == 0, result.stdout + result.stderr)

        overrides = json.dumps({'keeper': {'worker_threads': 4, 'heartbeat_interval_ms': 100,
                               'story_chunk_duration_secs': 3600, 'chunk_max_bytes': 67108864}})
        create_args = ['create', 'test', '--port-base', str(base), '--overrides', overrides]
        folder = root / 'registry/instances/test'
        processes = []
        all_pids = set()
        try:
            record = call(*create_args)
            if gate == 'bind_guard':
                require('insecure-bind-all' in call('create', 'bad', '--bind', '0.0.0.0', ok=False), 'bind refusal')
                require(call('create', 'unsafe', '--bind', '0.0.0.0', '--insecure-bind-all')['insecure_bind_all'], 'explicit insecure opt-in')
                for role in ('visor', 'keeper', 'grapher', 'player'):
                    key = 'internal_listen' if role == 'grapher' else 'listen'
                    require('insecure-bind-all' in call('create', 'bad', '--overrides',
                            json.dumps({role: {key: '0.0.0.0:31000'}}), ok=False), role + ' override guard')
            if gate == 'single_supervisor':
                for _ in range(2):
                    processes.append(subprocess.Popen([sys.executable, cli, 'up', 'test'], env=env,
                                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
                results = []
                for process in processes:
                    out, err = process.communicate(timeout=40)
                    require(process.returncode == 0, err)
                    results.append(json.loads(out))
                require(results[0]['supervisor_pid'] == results[1]['supervisor_pid'], 'one supervisor')
                require(results[0]['services'] == results[1]['services'], 'one service set')
                ready = results[0]
            else:
                ready = call('up', 'test')
            all_pids.update(s['pid'] for s in ready['services'].values())
            require(ready['state'] == 'ready', 'ready')
            helper('probe', ready)
            if gate == 'readiness':
                exported = subprocess.run([sys.executable, cli, 'env', 'test'], env=env,
                                          capture_output=True, text=True, timeout=5)
                require(exported.returncode == 0 and record['id'] in exported.stdout, 'env exports instance id')
                inherited = subprocess.run([sys.executable, cli, 'run', 'test', '--up', '--', sys.executable,
                    '-c', 'import json, os; print(json.dumps({k:v for k,v in os.environ.items() if k.startswith("CHRONOLOG_")}))'],
                    env=env, capture_output=True, text=True, timeout=5)
                require(inherited.returncode == 0, inherited.stderr)
                exported_env = json.loads(inherited.stdout)
                require(exported_env['CHRONOLOG_INSTANCE'] == record['id'] and
                        exported_env['CHRONOLOG_CATALOG'] == record['endpoints']['catalog'], 'run resolves and exports')
                call('register', 'external', '--catalog', record['endpoints']['catalog'],
                     '--player', record['endpoints']['player'])
                require(call('status', 'external')['state'] == 'ready', 'external discovery')
                s = ready['services']
                require(s['visor']['ready_ns'] <= min(s[r]['ready_ns'] for r in ('keeper', 'grapher')), 'Visor first')
                require(max(s[r]['ready_ns'] for r in ('keeper', 'grapher')) <= s['player']['ready_ns'] <= ready['ready_ns'], 'Player then probe')
                call('down', 'test')
                # An executable Player that exits before its line must never make up succeed.
                fake = root / 'bad-bin'
                fake.mkdir()
                from chronolog_local.registry import binary
                for role in ('visor', 'keeper', 'grapher'):
                    (fake / ('chrono_' + role)).symlink_to(binary(bin_dir, role))
                (fake / 'chrono_player').write_text('#!/bin/sh\nexit 1\n')
                (fake / 'chrono_player').chmod(0o700)
                require('player exited before readiness' in call('up', 'test', '--bin-dir', str(fake), ok=False), 'early Player exit')
            elif gate in ('restart_keeps_endpoints', 'supervisor_death'):
                if gate == 'restart_keeps_endpoints':
                    helper('durable', ready)
                os.kill(ready['supervisor_pid'], signal.SIGKILL)
                until(lambda: all(dead(pid) for pid in all_pids))
                require(call('ls')[0]['state'] == 'stopped', 'lock overrides stale ready status')
                again = call('up', 'test')
                all_pids.update(s['pid'] for s in again['services'].values())
                require(again['endpoints'] == record['endpoints'], 'fixed endpoints')
                if gate == 'restart_keeps_endpoints':
                    helper('read', again)
                # An individual service crash recovers without changing the other service pids.
                os.kill(again['services']['keeper']['pid'], signal.SIGKILL)
                until(lambda: call('status', 'test')['services']['keeper']['restarts'] == 1)
                until(lambda: call('status', 'test')['state'] == 'ready')
                recovered = call('status', 'test')
                all_pids.add(recovered['services']['keeper']['pid'])
                require(all(recovered['services'][r]['pid'] == again['services'][r]['pid']
                            for r in ('visor', 'grapher', 'player')), 'service isolation')
            elif gate == 'ordered_stop':
                helper('mixed', ready)
                call('down', 'test')
                messages = [json.loads(line)['message'] for line in (folder / 'logs/supervisor.log').read_text().splitlines()]
                require([m for m in messages if m.startswith('stop ')] ==
                        ['stop player', 'stop keeper', 'stop grapher', 'stop visor'], 'stop order')
                require(not any(m.startswith('kill deadline') for m in messages), 'clean shutdown')
                again = call('up', 'test')
                all_pids.update(s['pid'] for s in again['services'].values())
                helper('read', again)
            elif gate == 'attach_leases':
                holder = subprocess.Popen([sys.executable, cli, 'run', 'test', '--', sys.executable,
                                           '-c', 'import time; time.sleep(60)'], env=env)
                processes.append(holder)
                until(lambda: call('status', 'test')['attach']['count'] == 1)
                require('foreign attach leases' in call('down', 'test', ok=False), 'foreign lease refusal')
                holder.kill()
                holder.wait(timeout=5)
                until(lambda: call('status', 'test')['attach']['count'] == 0)
                call('down', 'test')
                ephemeral = call('up', 'ephemeral', '--ephemeral', '--idle-grace-s', '1',
                                 '--port-base', str(base + 5))
                all_pids.update(s['pid'] for s in ephemeral['services'].values())
                until(lambda: call('status', 'ephemeral')['state'] == 'stopped', seconds=40)
            elif gate == 'bind_guard':
                for directory in [root / 'registry', *[p for p in (root / 'registry').rglob('*') if p.is_dir()]]:
                    require(directory.stat().st_mode & 0o777 == 0o700, 'directory permissions ' + str(directory))
                for path in (root / 'registry').rglob('*'):
                    if path.is_file():
                        require(path.stat().st_mode & 0o777 == 0o600, 'file permissions ' + str(path))
                for role in ('visor', 'keeper', 'grapher', 'player'):
                    config = json.loads((folder / 'config' / (role + '.json')).read_text())
                    for key in ('listen', 'internal_listen'):
                        if key in config:
                            require(config[key].startswith('127.0.0.1:'), 'loopback ' + role)
                call('down', 'test')
                sock = socket.socket()
                sock.bind(('127.0.0.1', base + 2))
                sock.listen()
                try:
                    require('fixed endpoint' in call('up', 'test', ok=False), 'collision refusal')
                    require(call('status', 'test')['endpoints'] == record['endpoints'], 'never remap')
                finally:
                    sock.close()
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
            for name in ('test', 'ephemeral'):
                result = subprocess.run([sys.executable, cli, 'down', name, '--force'], env=env,
                                        capture_output=True, timeout=210)
                if result.returncode and (root / 'registry/instances' / name).exists():
                    print(result.stderr.decode(), file=sys.stderr)
            until(lambda: all(dead(pid) for pid in all_pids), seconds=5)
        print('PASS local.' + gate)


if __name__ == '__main__':
    main()
