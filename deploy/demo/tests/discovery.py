import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[3]
    engine = sys.argv[1] if len(sys.argv) > 1 else 'podman'
    project = 'local5-grapher-' + str(os.getpid())
    demo = root / 'deploy/demo/chronolog-demo'
    cli = root / 'deploy/local/chronolog'
    python = root / 'build/smoke-venv/bin/python'
    assert os.environ.get('RBUILD_HELD') == 'stack', 'FAIL demo discovery requires the stack lock'
    with tempfile.TemporaryDirectory(prefix='chronolog-demo-discovery-') as scratch:
        env = dict(os.environ, CHRONOLOG_HOME=scratch)
        env.pop('CHRONOLOG_CATALOG', None)
        env.pop('CHRONOLOG_INSTANCE', None)

        def call(command, timeout=30):
            result = subprocess.run([str(arg) for arg in command], env=env, cwd=root,
                                    capture_output=True, text=True, timeout=timeout)
            assert result.returncode == 0, 'FAIL demo discovery command: ' + result.stdout + result.stderr
            return result.stdout

        def rows():
            return json.loads(call([python, cli, 'ls']))

        down = [demo, 'down', '--volumes', '--engine', engine, '--project', project]
        try:
            up = [demo, 'up', '--engine', engine, '--project', project]
            call(up, timeout=300)
            records = rows()
            assert len(records) == 1, 'FAIL compose registry count'
            record = records[0]
            assert record['kind'] == 'external' and record['state'] == 'ready' and record['probe'] == 'rpc', 'FAIL compose discovery readiness'
            assert record['engine'] == engine and record['project'] == project, 'FAIL compose metadata'
            assert record['owner']['uid'] == os.getuid() and record['owner']['created_by'] == 'chronolog-demo', 'FAIL compose owner'
            assert record['endpoints'] == {'catalog': '127.0.0.1:50051', 'player': '127.0.0.1:50054'}, 'FAIL compose endpoints'
            path = Path(scratch) / 'external' / (record['name'] + '.json')
            assert path.stat().st_mode & 0o777 == 0o600 and Path(scratch).stat().st_mode & 0o777 == 0o700, 'FAIL registry permissions'
            call(up, timeout=300)
            assert rows()[0]['id'] == record['id'], 'FAIL repeated up preserves external identity'
            container = call([engine, 'ps', '--filter', 'label=com.docker.compose.project=' + project,
                              '--filter', 'label=com.docker.compose.service=chrono-player', '-q']).strip()
            assert container and '\n' not in container, 'FAIL own Player container'
            call([engine, 'stop', '--time', '20', container], timeout=60)
            assert rows()[0]['state'] == 'stopped' and path.exists(), 'FAIL externally stopped service liveness'
            call(down, timeout=120)
            assert rows() == [] and not path.exists(), 'FAIL compose record removed after down'
            print('PASS demo.discovery: RPC ready, metadata and permissions, idempotent up, external stop, down removes record')
        finally:
            call(down, timeout=120)


if __name__ == '__main__':
    main()
