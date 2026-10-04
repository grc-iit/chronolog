import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chronolog_local.registry import atomic, load, lock
from chronolog_local.supervisor import Supervisor, configs
from chronolog_local.tiers import MARKER, add_marker, filesystem, probe_tier, tier_config_keys


def require(condition, message):
    if not condition:
        raise AssertionError('FAIL ' + message)


def worker(folder):
    os.umask(0o077)
    folder = Path(folder)
    fd = lock(folder / 'run/supervisor.lock')
    require(fd is not None, 'test supervisor owns the lock')

    def hung_probe(tier, deployment_id):
        if tier['name'] == 'nfs':
            with (folder / 'run/probe_calls').open('a') as calls:
                calls.write('probe\n')
            threading.Event().wait()
        return probe_tier(tier, deployment_id)

    try:
        Supervisor(folder, load(folder / 'instance.json'), hung_probe).run()
    finally:
        os.close(fd)


def main():
    if sys.argv[1] == 'worker':
        worker(sys.argv[2])
        return
    gate, cli, bin_dir = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='chronolog-tiers-') as temporary:
        root = Path(temporary)
        env = dict(os.environ, CHRONOLOG_HOME=str(root / 'registry'), CHRONOLOG_BIN_DIR=bin_dir)

        def call(*args, ok=True, timeout=40):
            result = subprocess.run([sys.executable, cli, *args], env=env,
                                    capture_output=True, text=True, timeout=timeout)
            require((result.returncode == 0) == ok, result.stderr or 'unexpected command success')
            return json.loads(result.stdout) if ok else result.stderr

        record = call('create', 'test', '--tier-io-timeout-ms', '50',
                      '--tier-probe-interval-ms', '50')
        folder = root / 'registry/instances/test'
        local = record['tiers'][0]
        local_marker = load(Path(local['root']) / MARKER)
        require(local_marker['deployment_id'] == record['deployment_id'] and
                local_marker['tier_uuid'] == local['tier_uuid'] and local_marker['rank'] == 0,
                'create writes the local marker')
        require(len(local_marker['tier_uuid']) == 32, '128-bit tier uuid')
        if gate == 'tier_add_refuses_a_root_of_another_file_system_type':
            source = root / 'source'
            source.mkdir()
            expected = add_marker({'name': 'nfs', 'rank': 1, 'kind': 'posix', 'root': str(source)},
                                  record['deployment_id'])
            with tempfile.TemporaryDirectory(prefix='chronolog-tiers-', dir='/dev/shm') as other:
                target = Path(other)
                fd = os.open(target, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    require(filesystem(fd)['f_type'] != expected['f_type'],
                            'gate needs tmpfs distinct from the build file system')
                finally:
                    os.close(fd)
                shutil.copyfile(source / MARKER, target / MARKER)
                original = (target / MARKER).read_bytes()
                error = call('tier', 'add', 'test', 'nfs', str(target), '--rank', '1',
                             '--kind', 'slow', ok=False)
                require('another file system type' in error, 'type mismatch refuses tier-add')
                require((target / MARKER).read_bytes() == original, 'refusal preserves marker')
                require(load(folder / 'instance.json') == record, 'refusal preserves registry')
                (target / MARKER).unlink()
                added = call('tier', 'add', 'test', 'nfs', str(target), '--rank', '1', '--kind', 'slow')
                require(added['f_type'] != expected['f_type'], 'explicit add records the new file system')
                require([tier['rank'] for tier in call('tier', 'ls', 'test')] == [0, 1], 'ordered tier table')
                current = load(folder / 'instance.json')
                require(tier_config_keys(current)['deployment_id'] == record['deployment_id'],
                        'future config emitter carries deployment id')
                for role in ('grapher', 'player'):
                    generated = configs(current)[role]
                    require(generated['tiers'] == tier_config_keys(current)['tiers'] and
                            generated['deployment_id'] == record['deployment_id'] and
                            generated['tiers'][0]['root'] == generated['archive_root'],
                            'the Grapher and the Player get the tier table')
                    require('migrate_enabled' not in generated, 'migration stays at its default, off')
                legacy = {key: value for key, value in current.items() if key != 'deployment_id'}
                require(all('tiers' not in configs(legacy)[role] for role in ('grapher', 'player')),
                        'an instance without a deployment id keeps a plain archive root')
        elif gate == 'status_survives_a_hung_tier':
            call('tier', 'add', 'test', 'nfs', str(root / 'slow'), '--rank', '1', '--kind', 'slow')
            marker_before = (root / 'slow' / MARKER).read_bytes()
            output = (root / 'worker.log').open('w')
            process = subprocess.Popen([sys.executable, __file__, 'worker', str(folder)],
                                       env=env, stdout=output, stderr=output, start_new_session=True)
            try:
                deadline = time.monotonic() + 30
                while not (folder / 'run/status.json').exists():
                    require(process.poll() is None, 'supervisor survived initial publish')
                    require(time.monotonic() < deadline, 'supervisor publishes with a hung tier')
                    time.sleep(0.01)
                ready = call('up', 'test')
                require(ready['state'] == 'ready', 'up proceeds while slow probe never returns')
                current = call('status', 'test', timeout=5)
                require(current['state'] == 'ready', 'status proceeds while slow probe never returns')
                slow = next(tier for tier in current['tiers'] if tier['name'] == 'nfs')
                while not slow.get('probe_error'):
                    require(time.monotonic() < deadline, 'probe deadline is published')
                    time.sleep(0.01)
                    current = call('status', 'test', timeout=5)
                    slow = next(tier for tier in current['tiers'] if tier['name'] == 'nfs')
                require(not slow['available'] and slow.get('probe_error') == 'tier I/O deadline',
                        'expired probe reports tier unavailable')
                require(call('ls', timeout=5)[0]['state'] == 'ready', 'ls survives hung probe')
                require(not call('tier', 'ls', 'test', timeout=5)[1]['available'], 'tier ls survives hung probe')
                require((folder / 'run/probe_calls').read_text() == 'probe\n', 'only one outstanding probe')
                require((root / 'slow' / MARKER).read_bytes() == marker_before, 'probe never writes marker')
                call('down', 'test', '--force')
                process.wait(timeout=5)
                require(process.returncode == 0, 'shutdown never joins hung daemon probe')
            finally:
                if process.poll() is None:
                    call('down', 'test', '--force')
                    process.wait(timeout=5)
                output.close()
                if process.returncode:
                    print((root / 'worker.log').read_text(), file=sys.stderr)
        print('PASS local.' + gate)


if __name__ == '__main__':
    main()
