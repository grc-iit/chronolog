import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from chronolog_local.registry import atomic, load, lock
from chronolog_local.supervisor import Supervisor, configs
from chronolog_local.tiers import (GRAPHER_STATUS, MARKER, TierProbe, add_marker, filesystem, grapher_views,
                                   probe_tier, tier_config_keys)


def require(condition, message):
    if not condition:
        raise AssertionError('FAIL ' + message)


def grapher_file(written, scrub=None, **fields):
    value = {'writer': 'grapher-1', 'migrate_enabled': False, 'migration_stopped': False,
             'pending_tier_deletions': {}, 'heartbeat_ms': 1000, 'written_at_unix_ms': written,
             'scrub': scrub or {'enabled': True, 'validated': 7, 'skipped': 1, 'lost': 2, 'rolled_back': 3,
                                'slow_failed': 4, 'through': 99, 'finished_at_unix_ms': 1700000000123,
                                'error': 'slow tier timeout'},
             'tiers': [{'name': 'local', 'rank': 0, 'available': True, 'used_bytes': 5, 'budget_bytes': 0,
                        'above_high': False},
                       {'name': 'nfs', 'rank': 1, 'available': True, 'used_bytes': 6, 'budget_bytes': 0,
                        'above_high': False}]}
    value.update(fields)
    return {key: item for key, item in value.items() if item is not None}


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

        timings = {'status_reports_the_graphers_tier_view': [],
                   'generated_configs_carry_budgets_and_tier_timings': [
                       '--tier-io-timeout-ms', '700', '--tier-probe-interval-ms', '900', '--budget-bytes', '1234567']}
        # The booted Grapher and Player read these timings too; the Grapher view gate keeps their defaults.
        record = call('create', 'test', *timings.get(gate, ['--tier-io-timeout-ms', '50',
                                                            '--tier-probe-interval-ms', '50']))
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
                generated = configs(current)
                require(generated['keeper']['deployment_id'] == record['deployment_id'],
                        'the Keeper stamps and checks its WAL with the deployment id')
                require(generated['grapher']['tier_status_file'] == GRAPHER_STATUS == 'run/grapher-tiers.json' and
                        'tier_status_file' not in generated['player'],
                        'only the Grapher gets the status file, under the instance folder')
                current['overrides'] = {'grapher': {'tier_status_file': str(target / 'status.json')}}
                try:
                    configs(current)
                except ValueError as refusal:
                    require('fixed by instance.json' in str(refusal), 'the status file path is not an override')
                else:
                    require(False, 'an override moved the status file')
                current['overrides'] = {}
                error = call('tier', 'add', 'test', 'outer', str(root), '--rank', '2', '--kind', 'slow', ok=False)
                require('must not contain the instance folder' in error and not (root / MARKER).exists(),
                        'a tier root above the instance folder is refused before any marker')
                legacy = {key: value for key, value in current.items() if key != 'deployment_id'}
                require(all('tiers' not in configs(legacy)[role] for role in ('grapher', 'player')) and
                        'tier_status_file' not in configs(legacy)['grapher'],
                        'an instance without a deployment id keeps a plain archive root')
                views, migration = grapher_views(folder, legacy, True)
                require(not migration['known'] and migration['reason'] == 'no tier table' and
                        all(not view['known'] for view in views), 'no tier table, no Grapher view')
        elif gate == 'generated_configs_carry_budgets_and_tier_timings':
            error = call('tier', 'add', 'test', 'nfs', str(root / 'slow'), '--rank', '1', '--kind', 'slow',
                         '--budget-bytes', '-1', ok=False)
            require('nonnegative' in error and not (root / 'slow').exists(), 'a negative tier budget is refused')
            for flag in ('--tier-io-timeout-ms', '--tier-probe-interval-ms'):
                error = call('create', 'other', flag, '60001', ok=False)
                require('at most 60000' in error, flag + ' above the Grapher bound is refused')
            call('tier', 'add', 'test', 'nfs', str(root / 'slow'), '--rank', '1', '--kind', 'slow',
                 '--budget-bytes', '4096')
            current = load(folder / 'instance.json')
            generated = configs(current)
            for role in ('grapher', 'player'):
                require([tier['budget_bytes'] for tier in generated[role]['tiers']] == [1234567, 4096] and
                        generated[role]['tier_io_timeout_ms'] == 700 and
                        generated[role]['tier_probe_interval_ms'] == 900,
                        role + ' gets the tier budgets and timings: ' + json.dumps(generated[role]))
            older = dict(current, tiers=[{key: value for key, value in tier.items() if key != 'budget_bytes'}
                                         for tier in current['tiers']])
            older.pop('tier_io_timeout_ms')
            older.pop('tier_probe_interval_ms')
            require(all(tier['budget_bytes'] == 0 for tier in configs(older)['grapher']['tiers']) and
                    configs(older)['grapher']['tier_io_timeout_ms'] == 1000 and
                    configs(older)['player']['tier_probe_interval_ms'] == 5000,
                    'a record without budgets or timings gets the documented defaults')
            for key in ('tier_io_timeout_ms', 'tier_probe_interval_ms'):
                current['overrides'] = {'grapher': {key: 5}}
                try:
                    configs(current)
                except ValueError as refusal:
                    require('fixed by instance.json' in str(refusal), key + ' is not an override')
                else:
                    require(False, 'an override changed ' + key)
            try:
                require(call('up', 'test')['state'] == 'ready', 'the Grapher and the Player accept the tier table')
                for role in ('grapher', 'player'):
                    require(load(folder / 'config' / (role + '.json'))['tiers'] == generated[role]['tiers'],
                            'the booted ' + role + ' was given the budgets')
                deadline = time.monotonic() + 30
                while True:
                    tiers = call('status', 'test', timeout=5)['tiers']
                    if all(tier['grapher'].get('known') for tier in tiers):
                        break
                    require(time.monotonic() < deadline, 'the Grapher publishes its view: ' + json.dumps(tiers))
                    time.sleep(0.05)
                require([tier['grapher']['budget_bytes'] for tier in tiers] == [1234567, 4096],
                        'the running Grapher applies the budgets: ' + json.dumps(tiers))
            finally:
                call('down', 'test', '--force')
        elif gate == 'a_probe_in_flight_keeps_the_last_result':
            now = [0.0]
            release = threading.Event()
            calls = []

            def scripted(tier, deployment_id):
                calls.append(tier['name'])
                if len(calls) == 2:
                    release.wait()
                return {'available': True, 'used_bytes': len(calls)}

            probe = TierProbe(local, None, 1000, 100, scripted, lambda: now[0])

            def settle():
                probe.thread.join(timeout=30)
                require(not probe.thread.is_alive(), 'a scripted probe returns')
                return probe.poll()

            require(probe.poll() == {'available': False, 'used_bytes': None}, 'no probe has completed yet')
            require(settle() == {'available': True, 'used_bytes': 1}, 'the first probe completes')
            now[0] = 0.2
            require(probe.poll() == {'available': True, 'used_bytes': 1} and probe.thread is not None,
                    'a probe that just started keeps the last result')
            now[0] = 1.1
            require(probe.poll() == {'available': True, 'used_bytes': 1}, 'a probe in flight keeps the last result')
            now[0] = 1.3
            require(probe.poll() == {'available': False, 'used_bytes': None, 'probe_error': 'tier I/O deadline'},
                    'a probe past its deadline is unavailable')
            release.set()
            now[0] = 1.4
            require(settle() == {'available': False, 'used_bytes': None, 'probe_error': 'tier I/O deadline'},
                    'a probe that returns after its deadline stays unavailable')
            now[0] = 1.6
            require(probe.poll()['available'] is False and probe.thread is not None,
                    'the next probe starts from the deadline verdict')
            require(settle() == {'available': True, 'used_bytes': 3}, 'a timely probe restores the tier')
        elif gate == 'grapher_status_goes_stale_without_a_heartbeat':
            call('tier', 'add', 'test', 'nfs', str(root / 'slow'), '--rank', '1', '--kind', 'slow')
            current = load(folder / 'instance.json')
            status_file = folder / GRAPHER_STATUS
            status_file.parent.mkdir(parents=True, exist_ok=True)
            now = 1_800_000_000_000

            def seen(value):
                status_file.write_text(json.dumps(value))
                return grapher_views(folder, current, True, now)

            for written in (now, now - 3000, now + 3000):
                views, migration = seen(grapher_file(written))
                require(migration['known'] and migration['written_at_unix_ms'] == written and
                        migration['heartbeat_ms'] == 1000 and migration['scrub'] == grapher_file(now)['scrub'] and
                        all(view['known'] and view['available'] and view['scrub'] == migration['scrub']
                            for view in views), 'a file within three heartbeats is known at ' + str(written - now))
            for written in (now - 3001, now + 3001, 0):
                views, migration = seen(grapher_file(written))
                require(migration == {'known': False, 'reason': 'stale'} and
                        all(view == {'known': False, 'reason': 'stale'} for view in views),
                        'a file beyond three heartbeats is stale at ' + str(written - now))
            for fields in ({'written_at_unix_ms': None}, {'heartbeat_ms': None}, {'heartbeat_ms': 0},
                           {'written_at_unix_ms': str(now)}, {'heartbeat_ms': True}):
                views, migration = seen(grapher_file(now, **fields))
                require(migration == {'known': False, 'reason': 'no heartbeat'} and
                        all(not view['known'] for view in views), 'no heartbeat in ' + json.dumps(fields))
            for scrub in ({'enabled': True}, dict(grapher_file(now)['scrub'], lost='2'),
                          dict(grapher_file(now)['scrub'], error=None)):
                views, migration = seen(grapher_file(now, scrub=scrub))
                require(migration == {'known': False, 'reason': 'unparsable'} and
                        all(not view['known'] for view in views), 'a bad scrub object is unparsable')
            views, migration = seen(grapher_file(now, scrub=None) | {'scrub': 'none'})
            require(migration == {'known': False, 'reason': 'unparsable'}, 'a scrub of another type is unparsable')
            views, migration = grapher_views(folder, current, True, now + 3001)
            require(migration['reason'] == 'stale', 'the same bytes go stale as the clock moves')
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
        elif gate == 'status_reports_the_graphers_tier_view':
            slow_root = root / 'slow'
            call('tier', 'add', 'test', 'nfs', str(slow_root), '--rank', '1', '--kind', 'slow')
            status_file = folder / GRAPHER_STATUS
            status_file.parent.mkdir(parents=True, exist_ok=True)
            # What an earlier Grapher left behind must not pass for the next one's view.
            # What an earlier Grapher left behind, however fresh, must not pass for the next one's view.
            status_file.write_text(json.dumps(grapher_file(time.time_ns() // 1_000_000, writer='old')))

            def wait_for(condition, message):
                deadline = time.monotonic() + 30
                while True:
                    current = call('status', 'test', timeout=5)
                    if condition(current):
                        return current
                    require(time.monotonic() < deadline, message + ': ' + json.dumps(current.get('tiers')))
                    time.sleep(0.05)

            def view(current, name):
                return next(tier for tier in current['tiers'] if tier['name'] == name)['grapher']

            try:
                require(call('up', 'test')['state'] == 'ready', 'the stack boots with a slow tier')
                require(load(folder / 'config/grapher.json')['tier_status_file'] == GRAPHER_STATUS,
                        'the booted Grapher was given the status file')
                current = wait_for(lambda c: c.get('tier_migration', {}).get('writer') == 'grapher-1',
                                   'status carries the running Grapher view')
                require(current['state'] == 'ready', 'ready with the Grapher view')
                migration = current['tier_migration']
                require({key: value for key, value in migration.items()
                         if key not in ('written_at_unix_ms', 'heartbeat_ms', 'scrub')} ==
                        {'known': True, 'writer': 'grapher-1', 'migrate_enabled': False,
                         'migration_stopped': False, 'pending_tier_deletions': {}} and
                        migration['heartbeat_ms'] == 10000 and isinstance(migration['written_at_unix_ms'], int) and
                        all(key in migration['scrub'] for key in ('finished_at_unix_ms', 'validated', 'lost',
                                                                  'rolled_back', 'slow_failed', 'error')),
                        'migration view of an idle Grapher: ' + json.dumps(migration))
                for name in ('local', 'nfs'):
                    seen = view(current, name)
                    require(seen['known'] and seen['available'] is True and seen['above_high'] is False and
                            seen['migrate_enabled'] is False and seen['migration_stopped'] is False and
                            isinstance(seen['used_bytes'], int) and seen['budget_bytes'] == 0 and
                            seen['scrub'] == migration['scrub'], 'the Grapher sees ' + name)
                listed = call('tier', 'ls', 'test', timeout=5)
                require([tier['name'] for tier in listed] == ['local', 'nfs'] and
                        all(tier['grapher']['known'] and tier['grapher']['available'] for tier in listed) and
                        all('available' in tier for tier in listed),
                        'tier ls carries the Grapher view next to the launcher probe')
                # A stopped Grapher stays the running, ready child but writes nothing, so the file is ours to choose.
                grapher_pid = current['services']['grapher']['pid']
                os.kill(grapher_pid, signal.SIGSTOP)
                try:
                    # A long heartbeat keeps the fresh file fresh however slowly a loaded host publishes it.
                    now = time.time_ns() // 1_000_000
                    status_file.write_text(json.dumps(grapher_file(now, heartbeat_ms=60000)))
                    current = wait_for(lambda c: c['tier_migration'].get('written_at_unix_ms') == now,
                                       'a fresh file is known')
                    scrub = grapher_file(now)['scrub']
                    require(current['state'] == 'ready' and current['tier_migration']['scrub'] == scrub and
                            all(view(current, name)['known'] and view(current, name)['scrub'] == scrub
                                for name in ('local', 'nfs')),
                            'status shows the scrub pass: ' + json.dumps(current['tier_migration']))
                    require(all(tier['grapher']['known'] and tier['grapher']['scrub'] == scrub
                                for tier in call('tier', 'ls', 'test', timeout=5)), 'tier ls shows the scrub pass')
                    status_file.write_text(json.dumps(grapher_file(now - 180001, heartbeat_ms=60000)))
                    current = wait_for(lambda c: not c['tier_migration']['known'], 'a stale file is unknown')
                    require(current['state'] == 'ready' and current['tier_migration']['reason'] == 'stale' and
                            all(tier['grapher'] == {'known': False, 'reason': 'stale'} for tier in current['tiers']) and
                            all(tier['grapher'] == {'known': False, 'reason': 'stale'}
                                for tier in call('tier', 'ls', 'test', timeout=5)),
                            'a stale file is unknown in status and tier ls')
                    status_file.write_text(json.dumps(grapher_file(None)))
                    wait_for(lambda c: c['tier_migration'] == {'known': False, 'reason': 'no heartbeat'},
                             'a file without a heartbeat is unknown')
                    status_file.write_text('{"tiers": [')
                    current = wait_for(lambda c: c['tier_migration'].get('reason') == 'unparsable',
                                       'a garbage file is unknown')
                    require(current['state'] == 'ready' and
                            all(tier['grapher'] == {'known': False, 'reason': 'unparsable'}
                                for tier in current['tiers']) and 'error' not in current,
                            'a garbage file is unknown, not an error and not healthy')
                    status_file.write_text(json.dumps(grapher_file(now, tiers=[
                        {'name': 'local', 'rank': 0, 'available': 'yes'}])))
                    wait_for(lambda c: c['tier_migration'] == {'known': False, 'reason': 'unparsable'} and
                             view(c, 'local') == {'known': False, 'reason': 'unparsable'},
                             'a file of another shape is unknown')
                    status_file.unlink()
                    current = wait_for(lambda c: c['tier_migration'].get('reason') == 'absent',
                                       'an absent file is unknown')
                    require(current['state'] == 'ready' and
                            all(tier['grapher'] == {'known': False, 'reason': 'absent'} for tier in current['tiers']) and
                            not any(tier['grapher']['known'] for tier in call('tier', 'ls', 'test', timeout=5)),
                            'an absent file is unknown in status and tier ls')
                finally:
                    os.kill(grapher_pid, signal.SIGCONT)
                # The slow tier loses its marker: the Grapher's next probe changes its view and rewrites the file.
                marker = (slow_root / MARKER).read_bytes()
                (slow_root / MARKER).unlink()
                current = wait_for(lambda c: view(c, 'nfs').get('available') is False,
                                   'the Grapher reports the slow tier unavailable')
                require(view(current, 'local')['known'] and view(current, 'local')['available'] is True and
                        current['tier_migration']['known'], 'local stays available in the Grapher view')
                (slow_root / MARKER).write_bytes(marker)
                wait_for(lambda c: view(c, 'nfs').get('available') is True and
                         next(tier for tier in c['tiers'] if tier['name'] == 'nfs')['available'],
                         'the Grapher and the launcher both see the slow tier again')
                grapher_pid = call('status', 'test', timeout=5)['services']['grapher']['pid']
                os.kill(grapher_pid, 9)
                wait_for(lambda c: c['services']['grapher']['pid'] != grapher_pid and
                         c['tier_migration'].get('writer') == 'grapher-1',
                         'a restarted Grapher publishes its own view')
            finally:
                stopped = call('down', 'test', '--force')
            require(stopped['state'] == 'stopped' and 'tier_migration' not in stopped and
                    all('grapher' not in tier for tier in stopped['tiers']),
                    'a stopped instance reports no Grapher view')
            views, migration = grapher_views(folder, load(folder / 'instance.json'), False)
            require(migration == {'known': False, 'reason': 'grapher not running'} and
                    all(not seen['known'] for seen in views), 'no running Grapher, no view')
        print('PASS local.' + gate)


if __name__ == '__main__':
    main()
