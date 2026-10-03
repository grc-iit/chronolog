import json
import os
from pathlib import Path
import platform
import signal
import statistics
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chronolog_local.registry import binary


def main():
    cli, bin_dir, driver, commit = sys.argv[1:]
    samples = {key: [] for key in ('first', 'clean', 'wal100')}
    hardware = {'host': platform.node(), 'machine': platform.machine(),
                'cpu': next(line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                            if line.startswith('model name')),
                'memory': Path('/proc/meminfo').read_text().splitlines()[0], 'build': 'Release', 'commit': commit}
    print(json.dumps(hardware), flush=True)
    for run in range(10):
        with tempfile.TemporaryDirectory(prefix='local1-boot-', dir=str(Path.home())) as state, \
                tempfile.TemporaryDirectory(prefix='local1-tier-', dir='/tmp') as archive:
            env = dict(os.environ, CHRONOLOG_HOME=state)
            overrides = json.dumps({'keeper': {'story_chunk_duration_secs': 3600,
                                     'chunk_max_bytes': 67108864, 'chunk_max_events': 65536}})

            def call(*args):
                started = time.monotonic_ns()
                result = subprocess.run([sys.executable, cli, *args], env=env, capture_output=True,
                                        text=True, timeout=210)
                wall = time.monotonic_ns() - started
                if result.returncode:
                    raise RuntimeError('FAIL ' + result.stderr)
                return json.loads(result.stdout), wall

            def sample(kind, record, wall):
                value = {'run': run + 1, 'ready_ms': (record['ready_ns'] - record['started_ns']) / 1e6,
                         'cli_ms': wall / 1e6}
                samples[kind].append(value)
                print(json.dumps(dict(kind=kind, **value)), flush=True)

            ready = None
            try:
                ready, wall = call('up', 'boot', '--bin-dir', bin_dir, '--local-root', archive,
                                   '--budget-bytes', str(200_000_000_000), '--overrides', overrides)
                sample('first', ready, wall)
                call('down', 'boot')
                ready, wall = call('up', 'boot')
                sample('clean', ready, wall)
                result = subprocess.run([driver, 'wal100', ready['endpoints']['catalog'],
                                         ready['endpoints']['player'], state + '/events'],
                                        capture_output=True, text=True, timeout=120)
                if result.returncode:
                    raise RuntimeError('FAIL ' + result.stdout + result.stderr)
                wal_bytes = sum(path.stat().st_size for path in Path(ready['paths']['wal_dir']).rglob('*') if path.is_file())
                if wal_bytes < 100 * 1024 * 1024:
                    raise RuntimeError('FAIL fewer than 100 MiB of WAL')
                os.kill(ready['supervisor_pid'], signal.SIGKILL)
                # Wait for kernel PDEATHSIG cleanup, never for a latency assertion.
                deadline = time.monotonic() + 5
                for service in ready['services'].values():
                    path = Path('/proc') / str(service['pid']) / 'stat'
                    while path.exists() and path.read_text().rsplit(')', 1)[1].split()[0] != 'Z':
                        if time.monotonic() >= deadline:
                            raise RuntimeError('FAIL orphan service')
                        time.sleep(0.01)
                ready, wall = call('up', 'boot')
                sample('wal100', ready, wall)
                print(json.dumps({'run': run + 1, 'wal_bytes': wal_bytes}), flush=True)
            finally:
                call('down', 'boot', '--force')
    summary = {}
    for kind, values in samples.items():
        summary[kind] = {metric: {'p50': statistics.median(v[metric] for v in values),
                                 'max': max(v[metric] for v in values)} for metric in ('ready_ms', 'cli_ms')}
    print(json.dumps({'hardware': hardware, 'counts': {key: len(value) for key, value in samples.items()},
                      'summary': summary}), flush=True)


if __name__ == '__main__':
    main()
