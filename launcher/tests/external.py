import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import time

from sdk_wheel import prepare_sdk

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chronolog_local.registry import free_ports


def main():
    wheels, cli, visor = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='chronolog-external-') as scratch:
        root = Path(scratch)
        wheel = prepare_sdk(root / 'sdk-wheels', Path(wheels))
        env = dict(os.environ, CHRONOLOG_HOME=str(root / 'registry'))
        env.pop('PYTHONPATH', None)

        def call(command):
            result = subprocess.run([str(arg) for arg in command], env=env,
                                    capture_output=True, text=True, timeout=60)
            assert result.returncode == 0, 'FAIL external command: ' + result.stdout + result.stderr
            return result.stdout

        call([sys.executable, '-m', 'venv', root / 'venv'])
        python = root / 'venv/bin/python'
        call([python, '-m', 'pip', 'install', '--no-index', '--no-deps', wheel])
        base = None
        for _ in range(3):
            candidate = 10000 + random.randrange(4400) * 5
            try:
                free_ports({str(i): f'127.0.0.1:{candidate + i}' for i in range(5)})
                base = candidate
                break
            except ValueError:
                pass
        assert base is not None, 'FAIL external test ports'
        endpoint = f'127.0.0.1:{base}'
        call([python, cli, 'register', 'catalog', '--catalog', endpoint])
        call([python, cli, 'register', 'wrong-rpc', '--catalog', endpoint, '--player', endpoint])

        def states():
            return {row['name']: row for row in json.loads(call([python, cli, 'ls']))}

        assert all(row['state'] == 'stopped' for row in states().values()), 'FAIL no listener must be stopped'
        config = root / 'visor.json'
        config.write_text(json.dumps({'listen': endpoint, 'internal_listen': f'127.0.0.1:{base + 1}',
                                     'db_path': str(root / 'catalog.sqlite'),
                                     'keepers': [{'process_id': 'keeper-1', 'endpoint': f'127.0.0.1:{base + 2}'}]}))
        log = root / 'visor.log'
        with log.open('w') as out:
            process = subprocess.Popen([visor, '--config', str(config)], stdout=out, stderr=out)
        try:
            deadline = time.monotonic() + 30
            while 'catalog ready' not in log.read_text():
                assert process.poll() is None and time.monotonic() < deadline, 'FAIL Visor readiness: ' + log.read_text()
                time.sleep(0.05)
            rows = states()
            assert rows['catalog']['state'] == 'ready' and rows['catalog']['probe'] == 'rpc', 'FAIL real Catalog probe'
            assert rows['wrong-rpc']['state'] == 'degraded' and rows['wrong-rpc']['probe_error'], 'FAIL reachable wrong RPC must be degraded'
        finally:
            process.terminate()
            process.wait(timeout=10)
        assert all(row['state'] == 'stopped' for row in states().values()), 'FAIL stopped service must be stopped'
        print('PASS launcher.external_liveness: no listener stopped, real RPC ready, reachable wrong RPC degraded, stopped service stopped')


if __name__ == '__main__':
    main()
