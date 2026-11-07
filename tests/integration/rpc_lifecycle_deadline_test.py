#!/usr/bin/env python3
"""Paused Keeper/public tail regression. Failure cleanup is fixture disposal,
not orderly shutdown, native retirement or a production kill fallback.
"""
import argparse
import importlib.util
import uuid
import json
import hashlib
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import time


def identity(pid):
    try:
        proc = Path('/proc') / str(pid)
        return (os.readlink(proc / 'exe'), (proc / 'cmdline').read_bytes(),
                (proc / 'stat').read_text().rsplit(')', 1)[1].split()[19], proc.stat().st_uid)
    except (OSError, IndexError):
        return None


def fixture_processes(work):
    """Only exact executables and dedicated config argv are even candidates."""
    result = {}
    binaries = {str((work / 'bin' / ('chrono-' + kind)).resolve()) for kind in
                ('visor', 'keeper', 'grapher', 'player')}
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        found = identity(int(proc.name))
        if not found or found[0] not in binaries:
            continue
        if found[3] != os.geteuid():
            raise RuntimeError('dedicated executable UID is not owned by this fixture')
        args = found[1].split(b'\0')
        if b'--config' not in args:
            raise RuntimeError('dedicated executable has unverified config argv')
        config = Path(os.fsdecode(args[args.index(b'--config')+1])).resolve()
        if config.parent != work / 'conf' or not config.name.startswith('chrono-'):
            raise RuntimeError('dedicated executable has unexpected config')
        result[int(proc.name)] = found
    return result


def open_pidfd(pid):
    if hasattr(os, 'pidfd_open'):
        return os.pidfd_open(pid)
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    operation = libc.pidfd_open
    operation.argtypes, operation.restype = (ctypes.c_int, ctypes.c_uint), ctypes.c_int
    fd = operation(pid, 0)
    if fd < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return fd


def send_pidfd(fd, sig):
    if hasattr(signal, 'pidfd_send_signal'):
        return signal.pidfd_send_signal(fd, sig)
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    operation = libc.pidfd_send_signal
    operation.argtypes = (ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint)
    operation.restype = ctypes.c_int
    if operation(fd, sig, None, 0) < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


def signal_owned(pid, expected, sig):
    if identity(pid) != expected:
        raise RuntimeError('fixture process identity changed: ' + str(pid))
    # Hold a pidfd after matching executable/config/start identity. This avoids
    # signaling a reused PID between verification and the actual signal.
    fd = open_pidfd(pid)
    try:
        if identity(pid) != expected:
            raise RuntimeError('fixture process identity changed during pidfd admission')
        send_pidfd(fd, sig)
    finally:
        os.close(fd)


def patch(value, protocol, trace, enable_tail=True):
    if isinstance(value, dict):
        for key, child in value.items():
            if key in ('protocol_conf', 'extraction_protocol'):
                value[key] = protocol
            elif key == 'live_tail_read' and enable_tail:
                value[key] = True
            elif trace and key in ('level', 'flushlevel'):
                value[key] = 'trace'
            else:
                patch(child, protocol, trace, enable_tail)
    elif isinstance(value, list):
        for child in value:
            patch(child, protocol, trace, enable_tail)


def result_line(probe, output, timeout):
    end = time.monotonic()+timeout
    while time.monotonic() < end:
        ready, _, _ = select.select([probe.stdout], [], [], max(0, end-time.monotonic()))
        if ready:
            line = probe.stdout.readline()
            if not line:
                raise RuntimeError('probe exited before result: ' + str(probe.poll()))
            output.write(line)
            output.flush()
            if line.startswith(('READY ', 'TAIL ', 'REPLAY ')):
                return line.strip()
    raise RuntimeError('public probe exceeded test deadline')


def tail(probe, output):
    probe.stdin.write('TAIL\n')
    probe.stdin.flush()
    line = result_line(probe, output, 8)
    if not line.startswith('TAIL '):
        raise RuntimeError('unexpected result: '+line)
    return tuple(map(int, line.split()[1:]))


def dispose_fixture(owned, work, logs):
    """Explicitly authorized test-only cleanup. Never discover/kill other jobs.

    All candidates were absent before this fixture launched. Each exact
    executable/config/start identity is checked again with a pidfd. Resume
    paused fixture daemons, save artifacts/data first, then dispose only those
    still matching identities. This is never called a production drain pass.
    """
    for pid, expected in owned.items():
        if identity(pid) == expected:
            signal_owned(pid, expected, signal.SIGCONT)
    if (work / 'monitor').exists():
        shutil.copytree(work / 'monitor', logs / 'before-fixture-cleanup-monitor', dirs_exist_ok=True)
    if (work / 'output').exists():
        shutil.copytree(work / 'output', logs / 'before-fixture-cleanup-output', dirs_exist_ok=True)
    disposed = []
    for pid, expected in owned.items():
        if identity(pid) == expected:
            signal_owned(pid, expected, signal.SIGKILL)
            disposed.append(pid)
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
    (logs / 'fixture-cleanup.json').write_text(json.dumps({
        'verified_new_fixture_pids': disposed, 'production_retirement_proof': False}, indent=2))


def run(args):
    work = Path(os.environ.get('WORK_DIR', str(Path.home()/'chronolog-install-bounded-rpc-monotonic/chronolog'))).resolve()
    build = Path(os.environ.get('BUILD_DIR', str(Path.home()/'chronolog-build/bounded-rpc-monotonic/Debug'))).resolve()
    managed = args.managed_empty_partial or args.managed_orderly or args.archive_recovery
    logs = Path(args.logs).resolve()
    logs.mkdir(parents=True, exist_ok=False)
    shutil.copy2(Path(__file__), logs/'executed-fixture.py')
    if args.isolated_runtime:
        installed = work
        work = logs/'runtime-work'
        (work/'conf').mkdir(parents=True)
        for name in ('bin','lib','tools'):
            (work/name).symlink_to(installed/name, target_is_directory=True)
        for name in ('default-chrono-conf.json','default-chrono-client-conf.json'):
            shutil.copy2(installed/'conf'/name, work/'conf'/name)
        (logs/'installed-runtime-source.json').write_text(json.dumps({
            'installed':str(installed), 'runtime_work':str(work), 'same_installed_artifacts':True}))
    # Adopt only this harness's descendant launch tree so orderly exit can
    # be proved with waitpid, not inferred from PID absence or buffered logs.
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # Linux PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'cannot retain fixture child exit status')
    descriptor = open_pidfd(os.getpid())
    try:
        send_pidfd(descriptor, 0)  # Fail before launching anything if unsupported.
    finally:
        os.close(descriptor)
    if fixture_processes(work):
        raise RuntimeError('dedicated tree already occupied; refusing config mutation or adoption')
    configs = [work/'conf/default-chrono-conf.json', work/'conf/default-chrono-client-conf.json']
    saved = [path.read_bytes() for path in configs]
    for path, original in zip(configs, saved):
        (logs/('original-'+path.name)).write_bytes(original)
    env = dict(os.environ)
    if args.protocol == 'ofi+tcp':
        env['FI_TCP_IFACE'] = 'lo'  # Only this all-loopback fixture.
    if args.archive_recovery:
        args.managed_orderly = True
        env['LD_PRELOAD'] = str(build/'tests/integration/watermark-replay/librpc_archive_write_hold.so')
        env['CHRONOLOG_TEST_ARCHIVE_HOLD'] = str(logs/'archive-write')
    owned, probe = {}, None
    probe_output = None
    recovery_result = None
    try:
        for path in configs:
            data = json.loads(path.read_text())
            patch(data, args.protocol, args.trace, args.archive_recovery or not (args.empty_orderly or managed))
            if args.dual_extractor and 'chrono_keeper' in data:
                extractors = data['chrono_keeper']['ExtractionModule']['extractors']
                for configured in extractors.values():
                    if configured['type'] == 'single_endpoint_rdma_extractor':
                        configured['type'] = 'dual_endpoint_rdma_extractor'
                        configured['grapher_receiving_endpoint'] = configured.pop('receiving_endpoint')
                        configured['player_receiving_endpoint'] = dict(data['chrono_player']['PlayerRecordingService']['rpc'])
            path.write_text(json.dumps(data, indent=2)+'\n')
        if managed:
            previous = logs/'previous-lifecycle-files'
            previous.mkdir()
            # The exact dedicated tree was proved unoccupied before mutation.
            # Preserve old fixture proof files before the new deployment ID.
            for pattern in ('chrono-*.json.ready.json', 'chrono-*.json.publisher-park.json',
                            'chrono-*.json.retirement-permit.json', 'chrono-*.json.park-ack.json',
                            'chrono-*.json.stop-evidence.json', 'chrono-*.json.public-quiesced.json'):
                for path in (work/'conf').glob(pattern):
                    shutil.copy2(path, previous/path.name)
        with (logs/'deploy.log').open('w') as output:
            subprocess.run([str(work/'tools/deploy/deploy_local.sh'), '-d', '-w', str(work), '-k', '1', '-r', '1'],
                           stdout=output, stderr=subprocess.STDOUT, env=env, check=True, timeout=30)
        owned = fixture_processes(work)
        if len(owned) != 4:
            raise RuntimeError('expected four newly launched exact fixture daemons')
        keepers = [(pid, value) for pid, value in owned.items() if value[0].endswith('/chrono-keeper')]
        if len(keepers) != 1:
            raise RuntimeError('expected one owned Keeper')
        pid, expected = keepers[0]
        (logs/'process-identities.json').write_text(json.dumps({
            key: [v[0], os.fsdecode(v[1]), v[2], v[3]] for key, v in owned.items()}, indent=2))
        (logs/'binary-sha256.json').write_text(json.dumps({
            executable: hashlib.sha256(Path(executable).read_bytes()).hexdigest()
            for executable in {value[0] for value in owned.values()}}, indent=2))
        mappings = {}
        for process, expected_identity in owned.items():
            paths = {line.split()[-1] for line in Path('/proc', str(process), 'maps').read_text().splitlines()
                     if 'libchrono_rpc.so' in line}
            mappings[str(process)] = sorted(paths)
            if len(paths) != 1:
                raise RuntimeError('expected one physical shared chrono_rpc image per daemon')
        (logs/'runtime-mappings.json').write_text(json.dumps(mappings, indent=2))
        generated = []
        for path in sorted({Path(os.fsdecode(value[1].split(b'\0')[value[1].split(b'\0').index(b'--config')+1]))
                            for value in owned.values()}):
            def verify(value):
                if isinstance(value, dict):
                    for key, child in value.items():
                        if key in ('protocol_conf', 'extraction_protocol'):
                            if child != args.protocol:
                                raise RuntimeError('generated protocol mismatch: '+str(path))
                            generated.append([str(path), key, child])
                        else:
                            verify(child)
                elif isinstance(value, list):
                    for child in value:
                        verify(child)
            verify(json.loads(path.read_text()))
        (logs/'generated-protocols.json').write_text(json.dumps(generated, indent=2))
        if managed:
            helper = work/'tools/deploy/deployment_lifecycle.py'
            spec = importlib.util.spec_from_file_location('managed_deploy', helper)
            lifecycle = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(lifecycle)
            deadline = time.monotonic()+30
            # Consume the actual wrapper's original launch ledger. The fixture
            # must not synthesize/overwrite a deployment identity after launch.
            manifest = json.loads((work/'monitor/lifecycle/deployment.json').read_text())
            records = manifest['records']
            if manifest.get('state') != 'running' or {r['pid'] for r in records} != set(owned):
                raise RuntimeError('wrapper did not retain the exact four launch identities')
            env['CHRONOLOG_DEPLOYMENT_ID'] = manifest['id']
            for record in records:
                process = record['pid']
                if not lifecycle.matching(lifecycle.process_identity(process), record):
                    raise RuntimeError('wrapper launch identity changed before stop')
                state = json.loads(Path(record['status_file']).read_text())
                if (state.get('deployment_id') != manifest['id'] or state.get('state') != 'Running'
                    or state.get('pid') != process):
                    raise RuntimeError('wrapper returned before actual same-identity Running')
            (logs/'wrapper-launch-manifest.json').write_text(json.dumps(manifest, indent=2))
            if args.prequiesce_grapher:
                record = next(r for r in records if r['component'] == 'grapher')
                signal_owned(record['pid'], owned[record['pid']], signal.SIGTERM)
                until = time.monotonic()+5
                while True:
                    state = json.loads(Path(record['status_file']).read_text())
                    if state.get('state') == 'Draining' and state.get('status') == -27:
                        break
                    if time.monotonic() >= until:
                        raise RuntimeError('direct Grapher TERM did not preserve incomplete dependency drain')
                    time.sleep(.02)
                if state.get('native_proof',{}).get('finalized_mids') != 0:
                    raise RuntimeError('direct Grapher TERM retired a live Keeper dependency')
                (logs/'prequiesced-grapher.json').write_text(json.dumps(state, indent=2))
            def prequiesce_player():
                record = next(r for r in records if r['component']=='player')
                signal_owned(record['pid'],owned[record['pid']],signal.SIGTERM)
                until = time.monotonic()+5
                while True:
                    state = json.loads(Path(record['status_file']).read_text())
                    if state.get('state') == 'Draining' and state.get('status') == -27:
                        break
                    if time.monotonic() >= until:
                        raise RuntimeError('direct Player TERM did not retain required ingestion dependencies')
                    time.sleep(.02)
                proof = state.get('native_proof',{})
                if proof.get('initialized_mids') != 3 or proof.get('finalized_mids') != 0:
                    raise RuntimeError('direct Player TERM lacks actual retained3MID proof')
                (logs/'prequiesced-player.json').write_text(json.dumps(state,indent=2))
            if args.prequiesce_player and not args.archive_recovery:
                prequiesce_player()
            exits = {}
            if args.archive_recovery:
                probe_output = (logs/'recovery-probe.log').open('w')
                chronicle = 'LifecycleRecovery-'+str(owned[records[0]['pid']][2])
                if args.dual_extractor and args.prequiesce_player:
                    duration = json.loads(configs[0].read_text())['chrono_keeper']['DataStoreInternals']['story_chunk_duration_secs']
                    if duration != 10:
                        raise RuntimeError('dual fixture keeps the shipped10s Keeper chunk window')
                    period = duration*1000000000
                    begin = (time.time_ns()//period+1)*period+50000000
                    while time.time_ns()<begin:
                        time.sleep(max(0,min(.02,(begin-time.time_ns())/1e9)))
                probe = subprocess.Popen([str(build/'tests/integration/watermark-replay/chronolog-test-rpc-lifecycle-probe'),
                    str(configs[1]), '--recovery', chronicle],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=probe_output, text=True, bufsize=1, env=env)
                ready_line = result_line(probe, probe_output, 30)
                ready_fields = ready_line.split()
                if len(ready_fields) != 4 or ready_fields[:2] != ['READY','8']:
                    raise RuntimeError('recovery writer did not acknowledge all eight events: '+ready_line)
                last_event_ns = int(ready_fields[3])
                if args.prequiesce_player:
                    prequiesce_player()
                visible_until = time.monotonic()+15
                while True:
                    visible = tail(probe, probe_output)
                    if visible[0] == 0 and visible[2:] == (8,1):
                        break
                    if time.monotonic() >= visible_until:
                        raise RuntimeError('accepted data was not readable before archive outage: '+repr(visible))
                    time.sleep(.05)
                # Complete every original client-native cleanup before service
                # quiescence. Later data verification issues no public query.
                probe.stdin.write('QUIT\n')
                probe.stdin.flush()
                probe.wait(timeout=35)
                remainder = probe.stdout.read()
                probe_output.write(remainder)
                probe_output.flush()
                identities = [line for line in remainder.splitlines() if line.startswith('EVENT_ID ')]
                if probe.returncode != 0 or len(identities) != 8 or 'CLIENT_NATIVE_CLEANUP 1' not in remainder:
                    raise RuntimeError('public producer/reader did not settle original native work before quiescence')
                expected_file = logs/'expected-archive-identities.txt'
                expected_file.write_text('\n'.join(identities)+'\n')
                marker = logs/'archive-write.entered'
                entered_until = time.monotonic()+125
                while not marker.exists():
                    if time.monotonic() >= entered_until:
                        raise RuntimeError('actual archive write never entered the test-only hold')
                    time.sleep(.05)
                held_pid = int(marker.read_text().split()[0])
                if held_pid != next(r['pid'] for r in records if r['component'] == 'grapher'):
                    raise RuntimeError('archive hold did not belong to original Grapher')
                first_log = logs/'incomplete-stop.log'
                first_entered = time.monotonic()
                with first_log.open('w') as output:
                    first = subprocess.Popen([str(work/'tools/deploy/deploy_local.sh'), '-s', '-w', str(work)],
                        stdout=output, stderr=subprocess.STDOUT, env=env)
                    first.wait(timeout=33)
                first_elapsed = int((time.monotonic()-first_entered)*1000)
                first_rows = [json.loads(line) for line in first_log.read_text().splitlines() if line.startswith('{')]
                first_aggregate = [row for row in first_rows if row.get('aggregate')]
                if (first.returncode != 27 or not first_aggregate or first_aggregate[-1]['status'] != -27
                    or not 28500 <= first_elapsed <= 33000):
                    raise RuntimeError('held archive stop did not preserve original default30s boundary: '+repr(first_rows))
                retained = []
                for record in records:
                    if not lifecycle.matching(lifecycle.process_identity(record['pid']), record):
                        raise RuntimeError('incomplete stop lost/replaced original service identity')
                    state = json.loads(Path(record['status_file']).read_text())
                    if state.get('native_proof',{}).get('finalized_mids') != 0:
                        raise RuntimeError('incomplete archive stop retired a needed MID')
                    retained.append(state)
                keeper_record = next(r for r in records if r['component'] == 'keeper')
                if Path(keeper_record['config']+'.ready.json').exists() or (logs/'archive-write.completed').exists():
                    raise RuntimeError('readiness/archive confirmation preceded original held write')
                stop_identity = json.loads((work/'monitor/lifecycle/deployment.json').read_text())['stop_attempt']
                original_configs = {r['config']:Path(r['config']).read_bytes() for r in records}
                refused = subprocess.run([str(work/'tools/deploy/deploy_local.sh'), '-d','-w',str(work)],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True, timeout=3)
                (logs/'refused-overlap-start.log').write_text(refused.stdout)
                if refused.returncode != 27 or any(Path(path).read_bytes() != value for path,value in original_configs.items()):
                    raise RuntimeError('incomplete deployment allowed replacement start/config mutation')
                (logs/'incomplete-retained-native.json').write_text(json.dumps(retained, indent=2))
                (logs/'archive-write.release').touch()
                completed_until = time.monotonic()+30
                while not (logs/'archive-write.completed').exists():
                    if time.monotonic() >= completed_until:
                        raise RuntimeError('original held archive continuation did not complete after release')
                    time.sleep(.02)
                if (logs/'archive-write.completed').read_text().strip() != '0':
                    raise RuntimeError('original HDF5 write failed after release')
                # The production offline reader has no Player RPC acceptance
                # policy. It reads the actual manifest/files without RPC/Margo.
                archive_until = time.monotonic()+30
                while True:
                    archived = subprocess.run([str(build/'tests/integration/watermark-replay/chronolog-test-rpc-lifecycle-archive-probe'),
                        str(work/'output'), chronicle, 'retained', str(expected_file)],
                        capture_output=True, text=True, timeout=max(.001, archive_until-time.monotonic()), env=env)
                    (logs/'offline-archive-probe.log').write_text(archived.stdout+archived.stderr)
                    lines = [line for line in archived.stdout.splitlines() if line.startswith('ARCHIVE ')]
                    if archived.returncode == 0 and lines and lines[-1] == 'ARCHIVE 0 8 1 1 1':
                        break
                    if time.monotonic() >= archive_until:
                        raise RuntimeError('actual offline archive identity/payload proof failed: '+archived.stdout+archived.stderr)
                    time.sleep(.02)
                recovery_result = dict(first_stop_elapsed_ms=first_elapsed, first_aggregate=first_aggregate[-1],
                    original_held_grapher_pid=held_pid, original_hdf5_return=0,
                    public_before_quiescence=visible, client_native_cleanup=True,
                    archive_identity_payload_exact=True, archive_events=8, offline_abt_uninitialized=True,
                    original_player_guard_unchanged=True, stop_attempt=stop_identity)
                (logs/'archive-before-final-stop.json').write_text(json.dumps({str(path):hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in (work/'output').rglob('*.h5')}, indent=2))
            with (logs/'managed-stop.log').open('w') as output:
                controller = subprocess.Popen([str(work/'tools/deploy/deploy_local.sh'), '-s', '-w', str(work)],
                    stdout=output, stderr=subprocess.STDOUT, env=env)
                entered = time.monotonic()
                while controller.poll() is None:
                    # Reap only this test's verified original descendants. A
                    # zombie PID is not authoritative process absence.
                    for process in owned:
                        if process not in exits:
                            waited, wait_status = os.waitpid(process, os.WNOHANG)
                            if waited:
                                exits[process] = wait_status
                                (logs/'managed-wait-statuses.json').write_text(json.dumps(exits, indent=2))
                    if time.monotonic()-entered > 33:
                        controller.terminate()
                        controller.wait(timeout=3)
                        raise RuntimeError('managed controller exceeded its one30s budget')
                    time.sleep(.005)
            for process in owned:
                if process not in exits:
                    waited, wait_status = os.waitpid(process, os.WNOHANG)
                    if waited:
                        exits[process] = wait_status
            (logs/'managed-wait-statuses.json').write_text(json.dumps(exits, indent=2))
            if recovery_result and json.loads((work/'monitor/lifecycle/deployment.json').read_text())['stop_attempt'] != recovery_result['stop_attempt']:
                raise RuntimeError('later caller replaced the original managed drain identity')
            results = []
            for record in records:
                kind = record['component']
                if kind == 'visor' and not args.managed_orderly:
                    continue  # Historical partial fixture against the old main.
                state = json.loads(Path(record['status_file']).read_text())
                proof = state.get('native_proof', {})
                mids = 2 if kind in ('grapher', 'visor') else 3
                status = exits.get(record['pid'])
                if not (state.get('state') == 'Stopped' and state.get('status') == 0
                        and status == 0 and proof.get('initialized_mids') == proof.get('finalized_mids') == mids
                        and proof.get('finalize_calls') == proof.get('finalize_returns') == mids
                        and proof.get('constructor_units_created') == proof.get('constructor_units_retired') == mids
                        and proof.get('pools_created') == proof.get('pools_retired') == mids
                        and proof.get('rpc_streams_created') == proof.get('rpc_streams_retired')
                        and proof.get('application_handles') == 0 and proof.get('primary_releases') == 1
                        and proof.get('streams_before_release') == 1 and proof.get('same_primary') is True
                        and proof.get('abt_uninitialized') is True and proof.get('complete') is True):
                    raise RuntimeError(kind+' partial managed stop lacks physical original-generation release; wait_status='+repr(status)+' state='+repr(state))
                results.append({'component':kind, 'state':state, 'wait_status':status})
            rows = [json.loads(line) for line in (logs/'managed-stop.log').read_text().splitlines()
                    if line.startswith('{')]
            aggregate = [row for row in rows if row.get('aggregate')]
            expected_return, expected_status = (0, 0) if args.managed_orderly else (27, -27)
            if controller.returncode != expected_return or not aggregate or aggregate[-1]['status'] != expected_status:
                raise RuntimeError('managed aggregate mismatch: '+repr(aggregate))
            if args.dual_extractor and args.prequiesce_player:
                player_log = (work/'monitor/chrono-player-1.log').read_text()
                stopped = player_log.find('[ChronoPlayer] stop attempt 1 incomplete')
                received = [line for line in player_log[stopped:].splitlines()
                            if 'Received chunk for StoryID=' in line and 'eventCount8' in line]
                if stopped<0 or not received:
                    raise RuntimeError('dual Player ingestion after query closure was not observed')
                (logs/'dual-input-after-query-closure.json').write_text(json.dumps({
                    'direct_stop_incomplete_seen':True,'received_event_count':8,'lines':received},indent=2))
            (logs/'managed-empty-partial-results.json').write_text(json.dumps({
                'results':results, 'all_wait_statuses':exits, 'aggregate':aggregate[-1],
                'visor_native_retirement_proved':args.managed_orderly, 'recovery':recovery_result}, indent=2))
            print(json.dumps({'case':'managed_archive_recovery' if args.archive_recovery else ('managed_empty_native_retirement' if args.managed_orderly else 'managed_empty_partial_native_retirement'),
                              'protocol':args.protocol, 'status':'PASS' if args.managed_orderly else 'PARTIAL_PASS',
                              'results':results, 'trace':args.trace, 'aggregate_status':expected_status,
                              'visor_native_retirement_proved':args.managed_orderly, 'recovery':recovery_result}), flush=True)
            return
        if args.empty_orderly:
            results = []
            for kind in ('keeper', 'player'):
                matches = [(process, value) for process, value in owned.items() if value[0].endswith('/chrono-'+kind)]
                process, value = matches[0]
                arguments = value[1].split(b'\0')
                config_path = Path(os.fsdecode(arguments[arguments.index(b'--config')+1]))
                status_path = Path(str(config_path)+'.lifecycle.json')
                ready_deadline = time.monotonic()+30
                while True:
                    state = json.loads(status_path.read_text()) if status_path.exists() else {}
                    if state.get('pid') == process and state.get('start_ticks') == value[2] and state.get('state') == 'Running':
                        break
                    if time.monotonic() >= ready_deadline:
                        raise RuntimeError(kind+' never published its actual Running identity')
                    time.sleep(.01)
                entered = time.monotonic()
                signal_owned(process, value, signal.SIGTERM)
                while identity(process) == value:
                    if time.monotonic()-entered > 33:
                        raise RuntimeError(kind+' empty stop did not retire its native runtime')
                    time.sleep(.01)
                waited, wait_status = os.waitpid(process, 0)
                if waited != process or not os.WIFEXITED(wait_status) or os.WEXITSTATUS(wait_status) != 0:
                    raise RuntimeError(kind+' did not exit normally after native retirement: '+str(wait_status))
                state = json.loads(status_path.read_text())
                if not (state.get('pid') == process and state.get('start_ticks') == value[2]
                        and state.get('state') == 'Stopped' and state.get('status') == 0):
                    raise RuntimeError(kind+' exit lacks matching normal-code Stopped evidence')
                physical = state.get('native_proof', {})
                if not (physical.get('initialized_mids') == 3 and physical.get('finalize_calls') == 3
                        and physical.get('finalize_returns') == 3 and physical.get('finalized_mids') == 3
                        and physical.get('constructor_units_created') == physical.get('constructor_units_retired') == 3
                        and physical.get('rpc_streams_created') == physical.get('rpc_streams_retired')
                        and physical.get('pools_created') == physical.get('pools_retired') == 3
                        and physical.get('application_handles') == 0 and physical.get('primary_releases') == 1
                        and physical.get('streams_before_release') == 1 and physical.get('same_primary') is True
                        and physical.get('abt_uninitialized') is True and physical.get('complete') is True):
                    raise RuntimeError(kind+' lacks actual physical native retirement evidence: '+repr(physical))
                results.append({'component':kind, 'state':state, 'observed_ms':int((time.monotonic()-entered)*1000), 'wait_status':wait_status})
            (logs/'empty-orderly-results.json').write_text(json.dumps(results, indent=2))
            print(json.dumps({'case':'empty_orderly_native_retirement','protocol':args.protocol,
                              'status':'PASS','results':results,'trace':args.trace}), flush=True)
            return
        with (logs/'probe.log').open('w') as output, (logs/'probe.stderr').open('w') as error:
            probe = subprocess.Popen([str(build/'tests/integration/watermark-replay/chronolog-test-rpc-lifecycle-probe'), str(configs[1])],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error,
                                     text=True, bufsize=1, env=env)
            if result_line(probe, output, 15) != 'READY 8':
                raise RuntimeError('public probe did not acknowledge eight writes')
            statuses = {}
            for process, value in owned.items():
                arguments = value[1].split(b'\0')
                config_path = Path(os.fsdecode(arguments[arguments.index(b'--config')+1]))
                path = Path(str(config_path)+'.lifecycle.json')
                if path.exists():
                    observed = json.loads(path.read_text())
                    if observed['pid'] != process or observed['start_ticks'] != value[2]:
                        raise RuntimeError('daemon lifecycle status belongs to another start identity')
                    statuses[str(process)] = observed
            (logs/'running-statuses.json').write_text(json.dumps(statuses, indent=2))
            control_deadline = time.monotonic()+10
            while True:
                before = tail(probe, output)
                if before[0] == 0 and before[2:] == (8, 1):
                    break
                if before[0] not in (0, -24) or time.monotonic() >= control_deadline:
                    raise RuntimeError('control tail lacks original events: '+repr(before))
                time.sleep(.05)
            time.sleep(.05)  # Let the known control child retire before the pause.
            paused_at = time.monotonic()
            signal_owned(pid, expected, signal.SIGSTOP)
            expired = tail(probe, output)
            if expired[0] != -12 or not 4500 <= expired[1] < 6500 or expired[2] != 0:
                raise RuntimeError('default 5000 ms public tail violated: '+repr(expired))
            if args.client_exit_before_resume:
                # The existing public probe returns6 for this invalid command,
                # before Release/Disconnect. Its normal process exit closes the
                # transport without dispatching another metadata mutation.
                probe.stdin.write('EXIT_AFTER_TIMEOUT\n')
                probe.stdin.flush()
                probe.wait(timeout=5)
                if probe.returncode != 6:
                    raise RuntimeError('public client did not close the timed-out session')
            time.sleep(max(0, args.pause_secs-(time.monotonic()-paused_at)))
            signal_owned(pid, expected, signal.SIGCONT)
            time.sleep(1)
            if identity(pid) != expected:
                raise RuntimeError('Keeper exited/changed identity after late tail reply')
            if args.client_exit_before_resume:
                # A separate application process reads the original story and
                # appends nothing. Never replace an uncertain native context
                # inside the process that owned the original call.
                probe = subprocess.Popen([str(build/'tests/integration/watermark-replay/chronolog-test-rpc-lifecycle-probe'),
                    str(configs[1]), '--existing'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=error, text=True, bufsize=1, env=env)
                if result_line(probe, output, 15) != 'READY 8':
                    raise RuntimeError('separate public reader could not acquire original story')
            after = tail(probe, output)
            if after[0] != 0 or after[2:] != (8, 1):
                raise RuntimeError('retained data unavailable after resume: '+repr(after))
            probe.stdin.write('QUIT\n')
            probe.stdin.flush()
            probe.wait(timeout=10)
            if probe.returncode:
                raise RuntimeError('public probe failed: '+str(probe.returncode))
        print(json.dumps({'case':'paused_keeper_public_tail', 'protocol':args.protocol,
                          'status':'PASS', 'keeper_pid':pid, 'expired':expired,
                          'before':before, 'after':after, 'trace':args.trace, 'pause_secs':args.pause_secs, 'original_client_exited':args.client_exit_before_resume}), flush=True)
    finally:
        if args.archive_recovery:
            (logs/'archive-write.release').touch()
        if probe and probe.poll() is None:
            probe.terminate()  # Only the fixture's own client subprocess.
            try:
                probe.wait(timeout=3)
            except subprocess.TimeoutExpired:
                probe.kill()
                probe.wait(timeout=3)
        if probe_output:
            probe_output.close()
        try:
            # Deployment failure can leave original launches alive before the
            # normal post-launch inventory. They were absent at entry and use
            # only this fixture's dedicated exact executable/config identities.
            if not owned:
                owned = fixture_processes(work)
            snapshots = logs/'before-fixture-cleanup-config'
            snapshots.mkdir(exist_ok=True)
            for path in (work/'conf').glob('chrono-*.json*'):
                if path.is_file():
                    shutil.copy2(path, snapshots/path.name)
            dispose_fixture(owned, work, logs)
        finally:
            for path, original in zip(configs, saved):
                path.write_bytes(original)
                assert path.read_bytes() == original


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--protocol', choices=('ofi+sockets', 'ofi+tcp'), required=True)
    parser.add_argument('--logs', required=True)
    parser.add_argument('--trace', action='store_true')
    parser.add_argument('--pause-secs', type=float, default=45)
    parser.add_argument('--client-exit-before-resume', action='store_true')
    parser.add_argument('--empty-orderly', action='store_true')
    parser.add_argument('--managed-empty-partial', action='store_true')
    parser.add_argument('--managed-orderly', action='store_true')
    parser.add_argument('--archive-recovery', action='store_true')
    parser.add_argument('--isolated-runtime', action='store_true')
    parser.add_argument('--prequiesce-grapher', action='store_true')
    parser.add_argument('--prequiesce-player', action='store_true')
    parser.add_argument('--dual-extractor', action='store_true')
    run(parser.parse_args())
