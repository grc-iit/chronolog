#!/usr/bin/env python3
"""Deployment identity and one-budget stop controller. Services receive TERM only.

An overrun SSH/helper child belongs to this controller and may be terminated;
that is never a service stop fallback. Records/configs survive incomplete stop.
"""
import argparse
import errno
import hashlib
import fcntl
import uuid
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import sys
import time

INCOMPLETE = -27
COMPONENTS = ('keeper', 'player', 'grapher', 'visor')


def emit(value):
    print(json.dumps(value, separators=(',', ':')), flush=True)


def process_identity(pid):
    """None is proved absent; any other observation error is unknown."""
    proc = Path('/proc') / str(pid)
    try:
        fields = (proc / 'stat').read_text().rsplit(')', 1)[1].split()
        if fields[0] == 'Z':
            # The exact original PID still exists. Await its parent's reap;
            # never promote zombie observation to authoritative PID absence.
            return {'pid': pid, 'uid': proc.stat().st_uid, 'start_ticks': fields[19],
                    'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(), 'zombie': True}
        return {'pid': pid, 'uid': proc.stat().st_uid, 'executable': os.readlink(proc / 'exe'),
                'argv': [os.fsdecode(x) for x in (proc / 'cmdline').read_bytes().split(b'\0') if x],
                'start_ticks': fields[19],
                'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip()}
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ESRCH) and not proc.exists():
            return None
        raise


def matching(observed, record):
    return observed is not None and all(observed.get(key) == record.get(key)
        for key in ('pid', 'uid', 'executable', 'argv', 'start_ticks', 'boot_id'))



def original_zombie(observed, record):
    return observed is not None and observed.get('zombie') is True and all(
        observed.get(key) == record.get(key) for key in ('pid', 'uid', 'start_ticks', 'boot_id'))


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp-' + str(os.getpid()))
    with temporary.open('w') as output:
        json.dump(value, output)
        output.flush()
    os.replace(temporary, path)


def record_path(work, host, config):
    key = hashlib.sha256((host+'\0'+config).encode()).hexdigest()
    return Path(work)/'monitor/lifecycle'/(key+'.json')


class ExecArgumentsPending(RuntimeError):
    def __init__(self, observed):
        super().__init__('original launched process arguments are not yet observable')
        self.observed = observed


def record_process(work, pid, executable, config, component, expected=None):
    observed = process_identity(pid)
    config = str(Path(config).resolve())
    executable = str(Path(executable).resolve())
    if not observed or observed.get('uid') != os.geteuid() or observed.get('executable') != executable:
        raise RuntimeError('launched PID/executable identity is unverified')
    if expected is not None and any(observed.get(key) != expected.get(key)
            for key in ('pid','uid','executable','start_ticks','boot_id')):
        raise RuntimeError('original launched process lifetime changed before verification')
    args = observed['argv']
    if not args:
        # CLOEXEC can close Popen's exec-error pipe before the ELF loader has
        # populated /proc/PID/cmdline. This is still unverified, not Running.
        raise ExecArgumentsPending(observed)
    if '--config' not in args or args.index('--config')+1 >= len(args) or str(Path(args[args.index('--config')+1]).resolve()) != config:
        raise RuntimeError('launched PID/config identity is unverified: expected '+repr(config)+'; observed argv '+repr(args))
    observed.update({'host': socket.gethostname(), 'config': config, 'component': component,
                     'status_file': config+'.lifecycle.json', 'config_bytes': Path(config).read_text(),
                     'deployment_id': os.environ.get('CHRONOLOG_DEPLOYMENT_ID', '')})
    atomic_json(record_path(work, observed['host'], config), observed)
    return observed


def status(record):
    try:
        current = process_identity(record['pid'])
        if current is not None:
            return 'alive' if matching(current, record) or original_zombie(current, record) else 'unknown'
        # Absence is process exit; successful stop also needs its matching
        # normal-code completion publication, not a missing/buffered log line.
        stopped = json.loads(Path(record['status_file']).read_text())
        same = all(stopped.get(key) == record.get(key)
                   for key in ('host', 'pid', 'uid', 'executable', 'config', 'start_ticks', 'boot_id', 'deployment_id'))
        proof = stopped.get('native_proof', {})
        return 'stopped' if (same and stopped.get('state') == 'Stopped' and stopped.get('status') == 0
            and proof.get('complete') is True and proof.get('abt_uninitialized') is True
            and proof.get('application_handles') == 0) else 'unknown'
    except (OSError, ValueError, KeyError, IndexError):
        return 'unknown'



def inspect_process(record):
    state = status(record)
    result = {'state': state}
    if state == 'alive':
        try:
            current = json.loads(Path(record['status_file']).read_text())
            if same_identity(current, record) and current.get('deployment_id') == record.get('deployment_id'):
                result['native_generation'] = current.get('native_proof', {}).get('generation', 0)
            if record['component'] == 'keeper':
                ready_bytes = Path(record['config']+'.ready.json').read_text()
                validate_ready(record, ready_bytes, record['deployment_id'])
                # Recheck exact process identity after file observations.
                if matching(process_identity(record['pid']), record):
                    result['ready_bytes'] = ready_bytes
            elif record['component'] == 'visor':
                encoded = Path(record['config']+'.public-quiesced.json').read_text()
                validate_public(record, encoded, record['deployment_id'], result.get('native_generation', 0))
                if matching(process_identity(record['pid']), record):
                    result['public_quiesced_bytes'] = encoded
            elif record['component'] == 'player':
                encoded = Path(record['config']+'.queries-quiesced.json').read_text()
                validate_queries(record, encoded, record['deployment_id'], result.get('native_generation',0))
                if matching(process_identity(record['pid']), record):
                    result['queries_quiesced_bytes'] = encoded
            elif record['component'] == 'grapher':
                encoded = Path(record['config']+'.park-ack.json').read_text()
                if matching(process_identity(record['pid']), record):
                    result['park_ack_bytes'] = encoded
        except (OSError, ValueError, KeyError, RuntimeError):
            pass  # Missing readiness/ACK is not a completed barrier.
    elif state == 'stopped':
        encoded = Path(record['status_file']).read_text()
        if process_identity(record['pid']) is None and status(record) == 'stopped':
            result.update(authoritative_absent=True, terminal_bytes=encoded)
        else:
            result['state'] = 'unknown'
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


def signal_process(record):
    current = process_identity(record['pid'])
    if current is None:
        return status(record)
    if original_zombie(current, record):
        return 'alive'
    if not matching(current, record):
        return 'unknown'
    descriptor = open_pidfd(record['pid'])
    try:
        if not matching(process_identity(record['pid']), record):
            return 'unknown'
        send_pidfd(descriptor, signal.SIGTERM)
        return 'alive'
    finally:
        os.close(descriptor)


def inventory(work, executables=()):
    host = socket.gethostname()
    records = []
    for path in sorted((Path(work)/'monitor/lifecycle').glob('*.json')):
        record = json.loads(path.read_text())
        if record.get('host') == host:
            record['state'] = status(record)
            records.append(record)
    # Detect unrecorded or mismatched occupied deployment before any config
    # rewrite/start. A scan error for an expected process is not absence.
    expected = {str((Path(work)/'bin'/('chrono-'+kind)).resolve()) for kind in COMPONENTS}
    expected.update(str(Path(value).resolve()) for value in executables)
    expected.update(record['executable'] for record in records)
    for directory in Path('/proc').iterdir():
        if not directory.name.isdigit():
            continue
        try:
            executable = os.readlink(directory/'exe')
        except OSError as error:
            if error.errno in (errno.ENOENT, errno.ESRCH, errno.EACCES):
                continue
            raise
        if executable.removesuffix(' (deleted)') in expected:
            observed = process_identity(int(directory.name))
            if not any(matching(observed, record) for record in records):
                raise RuntimeError('unrecorded/mismatched deployment process is occupied')
    return {'host':host, 'records':records}


def call_worker(host, work, action, deadline, record=None):
    remaining = deadline-time.monotonic()
    if remaining <= 0:
        raise TimeoutError('aggregate stop budget expired')
    arguments = [sys.executable, str(Path(__file__).resolve()), '--work-dir', str(work),
                 'worker', action]
    if record is not None:
        arguments += ['--record', json.dumps(record, separators=(',', ':'))]
    if host not in ('local', 'localhost', '127.0.0.1', socket.gethostname()):
        arguments = ['ssh', '-n', '-oBatchMode=yes',
                     '-oConnectTimeout='+str(max(1, min(5, int(remaining)))),
                     host, shlex.join(arguments)]
    child = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        output, error = child.communicate(timeout=max(0, deadline-time.monotonic()))
        if child.returncode != 0:
            detail = error or output
            try:
                failure = json.loads(output)
            except json.JSONDecodeError:
                pass
            else:
                if isinstance(failure, dict) and isinstance(failure.get('reason'), str):
                    detail = failure['reason']
            raise RuntimeError('worker exit '+str(child.returncode)+': '+detail[-1024:])
        return json.loads(output)
    except BaseException:
        # This exact child is our SSH/helper, never a service PID. No service
        # KILL or another full wait is added after the aggregate expiry.
        if child.poll() is None:
            child.kill()
        raise



IDENTITY = ('host', 'pid', 'uid', 'executable', 'config', 'start_ticks', 'boot_id')
READY_GATES = ('write_admission_closed', 'admitted_ingest_complete', 'archive_confirmed',
               'joins_complete', 'outgoing_settled')


def same_identity(value, record):
    return all(value.get(key) == record.get(key) for key in IDENTITY)


def group(record):
    configuration = json.loads(Path(record['config']).read_text())
    return configuration['chrono_' + record['component']].get('RecordingGroup', 0)


def target(record):
    configuration = json.loads(Path(record['config']).read_text())
    endpoint = configuration['chrono_keeper']['KeeperDataStoreAdminService']['rpc']
    return (endpoint['protocol_conf']+'://'+endpoint['service_ip']+':'
            +str(endpoint['service_base_port'])+'@'+str(endpoint['service_provider_id']))



def visor_target(record):
    configured = json.loads(Path(record['config']).read_text())
    endpoint = configured['chrono_visor']['VisorKeeperRegistryService']['rpc']
    return (endpoint['protocol_conf']+'://'+endpoint['service_ip']+':'
            +str(endpoint['service_base_port'])+'@'+str(endpoint['service_provider_id']))


def player_target(record):
    configured = json.loads(Path(record['config']).read_text())
    endpoint = configured['chrono_player']['PlaybackQueryService']['rpc']
    return (endpoint['protocol_conf']+'://'+endpoint['service_ip']+':'
            +str(endpoint['service_base_port'])+'@'+str(endpoint['service_provider_id']))


def validate_queries(record, encoded, deployment, generation):
    value = json.loads(encoded)
    if (not same_identity(value, record) or value.get('deployment_id') != deployment
        or value.get('config_bytes') != Path(record['config']).read_text()
        or encoded != Path(record['config']+'.queries-quiesced.json').read_text()
        or value.get('state') != 'QueriesQuiesced' or value.get('status') != 0
        or not value.get('drain_id') or generation <= 0 or value.get('native_generation') != generation
        or value.get('target') != player_target(record)
        or not all(value.get(gate) is True for gate in
                   ('query_admission_closed','query_native_work_settled','keeper_dispatch_closed'))):
        raise RuntimeError('Player query-quiescence identity/native proof mismatch')
    return value


def quiesce_player_queries(work, records, manifest, deadline, worker, now, sleep):
    # Once every Keeper is normally stopped/absent there can be no later
    # Keeper TERM; completed-stop retries do not reopen or recreate this phase.
    if all(record['state'] == 'stopped' for record in records if record['component'] == 'keeper'):
        return []
    pending = []
    for record in (r for r in records if r['component'] == 'player'):
        observed = worker(record['_route'], work, 'inspect', deadline, record)
        generation = observed.get('native_generation',0)
        if observed['state'] != 'alive' or generation <= 0:
            raise RuntimeError('needed Player identity/native generation is unknown')
        packet = {'deployment_id':manifest['id'], 'stop_attempt':manifest['stop_attempt'],
                  'recipient':record, 'recipient_config_bytes':Path(record['config']).read_text(),
                  'recipient_native_generation':generation}
        atomic_json(record['config']+'.queries-quiesce.json', packet)
        pending.append(record)
    proofs = []
    while pending:
        if now() >= deadline:
            raise TimeoutError('aggregate stop budget expired waiting for Player queries')
        remaining = []
        for record in pending:
            observed = worker(record['_route'], work, 'inspect', deadline, record)
            if observed['state'] != 'alive':
                raise RuntimeError('Player did not retain ingestion dependencies for Keeper drain')
            encoded = observed.get('queries_quiesced_bytes')
            if not encoded:
                remaining.append(record)
                continue
            value = validate_queries(record, encoded, manifest['id'], observed.get('native_generation',0))
            if value.get('stop_attempt') != manifest['stop_attempt']:
                raise RuntimeError('Player query proof belongs to another drain')
            proofs.append({'record':record, 'proof_bytes':encoded,
                           'proof_path':record['config']+'.queries-quiesced.json'})
        pending = remaining
        if pending:
            sleep(min(.05, max(0,deadline-now())))
    return proofs


def validate_public(record, encoded, deployment, generation):
    value = json.loads(encoded)
    if (not same_identity(value, record) or value.get('deployment_id') != deployment
        or value.get('config_bytes') != Path(record['config']).read_text()
        or encoded != Path(record['config']+'.public-quiesced.json').read_text()
        or value.get('state') != 'PublicQuiesced' or value.get('status') != 0
        or not value.get('drain_id') or generation <= 0 or value.get('native_generation') != generation
        or value.get('target') != visor_target(record)
        or not all(value.get(gate) is True for gate in
                   ('public_admission_closed','native_work_settled','keeper_dispatch_closed'))):
        raise RuntimeError('Visor public-quiescence identity/native proof mismatch')
    return value


def quiesce_visors(work, records, manifest, deadline, worker, now, sleep):
    visors = [record for record in records if record['component'] == 'visor']
    pending = []
    for record in visors:
        if record['state'] == 'unknown':
            raise RuntimeError('Visor process identity is unknown')
        if record['state'] == 'stopped':
            if any(peer['state'] != 'stopped' for peer in records if peer['component'] != 'visor'):
                raise RuntimeError('Visor exited before its managed dependencies')
            continue
        record['state'] = worker(record['_route'], work, 'term', deadline, record)['state']
        if record['state'] == 'unknown':
            raise RuntimeError('Visor quiescence signal identity is unknown')
        pending.append(record)
    proofs = []
    while pending:
        if now() >= deadline:
            raise TimeoutError('aggregate stop budget expired waiting for public quiescence')
        remaining = []
        for record in pending:
            observed = worker(record['_route'], work, 'inspect', deadline, record)
            record['state'] = observed['state']
            if observed['state'] != 'alive':
                raise RuntimeError('Visor did not retain both MIDs for dependency drain')
            encoded = observed.get('public_quiesced_bytes')
            if not encoded:
                remaining.append(record)
                continue
            validate_public(record, encoded, manifest['id'], observed.get('native_generation', 0))
            proofs.append({'record':record, 'proof_bytes':encoded,
                           'proof_path':record['config']+'.public-quiesced.json'})
        pending = remaining
        if pending:
            sleep(min(.05, max(0, deadline-now())))
    return proofs


def validate_ready(record, encoded, deployment):
    ready = json.loads(encoded)
    if (not same_identity(ready, record) or ready.get('deployment_id') != deployment
        or ready.get('config_bytes') != Path(record['config']).read_text()
        or encoded != Path(record['config']+'.ready.json').read_text()
        or ready.get('state') != 'Ready' or ready.get('status') != 0
        or ready.get('group') != group(record) or ready.get('target') != target(record)
        or not ready.get('drain_id') or ready.get('native_generation', 0) <= 0
        or not all(ready.get(gate) is True for gate in READY_GATES)):
        raise RuntimeError('Keeper readiness identity/data proof mismatch')
    return ready


def progress_barrier(work, records, observations, manifest, deadline, worker, public_proofs, query_proofs=()):
    # Every input observation came from that identity's authoritative host.
    # Missing/failed observation never authorizes parking or a permit.
    ready = []
    for keeper in (r for r in records if r['component'] == 'keeper'):
        observation = observations.get(keeper['config'], {})
        encoded = observation.get('ready_bytes')
        if observation.get('state') != 'alive' or not encoded:
            continue
        proof = validate_ready(keeper, encoded, manifest['id'])
        ready.append({'record': keeper, 'ready_bytes': encoded, 'target': proof['target'],
                      'group': proof['group']})
    acknowledgments = {}
    for grapher in (r for r in records if r['component'] == 'grapher'):
        needed = [item for item in ready if item['group'] == group(grapher)]
        if not needed:
            continue
        observation = worker(grapher['_route'], work, 'inspect', deadline, grapher)
        if observation['state'] != 'alive' or observation.get('native_generation', 0) <= 0:
            raise RuntimeError('managed publisher identity/generation is unknown')
        packet = {'deployment_id': manifest['id'], 'stop_attempt': manifest['stop_attempt'],
                  'recipient': grapher, 'recipient_config_bytes': Path(grapher['config']).read_text(),
                  'recipient_native_generation': observation['native_generation'], 'keepers': needed}
        atomic_json(grapher['config']+'.publisher-park.json', packet)
        encoded = observation.get('park_ack_bytes')
        if not encoded:
            continue
        ack = json.loads(encoded)
        if (ack.get('deployment_id') != manifest['id'] or ack.get('stop_attempt') != manifest['stop_attempt']
            or not same_identity(ack.get('recipient', {}), grapher)
            or ack.get('recipient_config_bytes') != packet['recipient_config_bytes']
            or ack.get('recipient_native_generation') != observation['native_generation']
            or ack.get('state') != 'Parked' or ack.get('status') != 0):
            raise RuntimeError('publisher park acknowledgment identity mismatch')
        acknowledgments[grapher['config']] = (grapher, encoded, ack)
    for item in ready:
        publishers = []
        for grapher in (r for r in records if r['component'] == 'grapher' and group(r) == item['group']):
            observation = acknowledgments.get(grapher['config'])
            if not observation:
                break
            _, encoded, ack = observation
            matches = [entry for entry in ack.get('keepers', [])
                       if entry.get('target') == item['target'] and entry.get('ready_bytes') == item['ready_bytes']
                       and same_identity(entry.get('record', {}), item['record'])]
            if len(matches) != 1:
                break
            publishers.append({'record': grapher, 'ack_bytes': encoded,
                               'ack_path': grapher['config']+'.park-ack.json'})
        else:
            packet = {'deployment_id': manifest['id'], 'stop_attempt': manifest['stop_attempt'],
                      'recipient': item['record'], 'recipient_config_bytes': Path(item['record']['config']).read_text(),
                      'ready_bytes': item['ready_bytes'], 'publishers': publishers, 'visors': public_proofs,
                      'players':list(query_proofs)}
            atomic_json(item['record']['config']+'.retirement-permit.json', packet)


def retain_terminal(work, record, observation, manifest):
    encoded = observation.get('terminal_bytes')
    if observation.get('state') != 'stopped' or observation.get('authoritative_absent') is not True or not encoded:
        raise RuntimeError('dependency terminal/authoritative process absence is unconfirmed')
    value = json.loads(encoded)
    if (not same_identity(value, record) or value.get('state') != 'Stopped' or value.get('status') != 0
        or value.get('deployment_id') != manifest['id'] or encoded != Path(record['status_file']).read_text()
        or record['config_bytes'] != Path(record['config']).read_text()):
        raise RuntimeError('dependency terminal/config identity mismatch')
    native = value.get('native_proof', {})
    mids = 2 if record['component'] == 'grapher' else 3
    if (not all(native.get(flag) is True for flag in ('complete','abt_uninitialized','same_primary'))
        or native.get('application_handles') != 0 or native.get('primary_releases') != 1
        or native.get('streams_before_release') != 1 or native.get('generation', 0) <= 0
        or type(native.get('rpc_streams_created')) is not int or native['rpc_streams_created'] < 0
        or type(native.get('rpc_streams_retired')) is not int
        or native.get('rpc_streams_created') != native.get('rpc_streams_retired')
        or not all(native.get(field) == mids for field in ('initialized_mids','finalized_mids',
             'finalize_calls','finalize_returns','constructor_units_created','constructor_units_retired',
             'pools_created','pools_retired'))):
        raise RuntimeError('dependency physical native release is incomplete')
    digest = hashlib.sha256(encoded.encode()).hexdigest()
    retained = {key:value for key,value in record.items() if not key.startswith('_')}
    retained['terminal_sha256'] = digest
    path = record_path(work, record['host'], record['config'])
    atomic_json(path, retained)
    return {'record':retained, 'authoritative_absent':True, 'terminal_bytes':encoded,
            'terminal_sha256':digest, 'record_path':str(path), 'record_bytes':path.read_text(),
            'config_bytes':Path(record['config']).read_text()}


def terminal_evidence(work, records, observations, manifest):
    keepers = []
    for keeper in (r for r in records if r['component'] == 'keeper'):
        item = retain_terminal(work, keeper, observations.get(keeper['config'], {}), manifest)
        item['target'] = target(keeper)
        keepers.append(item)
    manifest_bytes = (Path(work)/'monitor/lifecycle/deployment.json').read_text()
    for grapher in (r for r in records if r['component'] == 'grapher' and r['state'] != 'stopped'):
        previous = json.loads(Path(grapher['status_file']).read_text())
        if not same_identity(previous, grapher):
            raise RuntimeError('Grapher status identity mismatch')
        packet = {'deployment_id':manifest['id'], 'stop_attempt':manifest['stop_attempt'],
                  'managed_fence_bytes':manifest_bytes, 'recipient':grapher,
                  'recipient_attempt':previous.get('attempt', 0)+1,
                  'recipient_config_bytes':Path(grapher['config']).read_text(), 'group':group(grapher),
                  'keepers':[item for item in keepers if group(item['record']) == group(grapher)]}
        atomic_json(grapher['config']+'.stop-evidence.json', packet)

    for player in (r for r in records if r['component'] == 'player' and r['state'] != 'stopped'):
        current = json.loads(Path(player['status_file']).read_text())
        if not same_identity(current,player):
            raise RuntimeError('Player status identity mismatch')
        generation = current.get('native_proof',{}).get('generation',0)
        if generation <= 0:
            raise RuntimeError('Player native generation is unknown')
        packet = {'deployment_id':manifest['id'], 'stop_attempt':manifest['stop_attempt'],
                  'recipient':player, 'recipient_config_bytes':Path(player['config']).read_text(),
                  'recipient_native_generation':generation, 'services':keepers,
                  'keeper_launches_closed':True}
        if keepers:
            packet['query_quiesced_bytes'] = Path(player['config']+'.queries-quiesced.json').read_text()
        else:
            if (manifest.get('inventory_complete') is not True or manifest.get('stop_inventory_complete') is not True
                or not manifest.get('hosts') or set(manifest['hosts']) != set(manifest.get('stop_inventory_routes',[]))
                or 'pending_launch' in manifest):
                raise RuntimeError('empty Keeper roster lacks closed authoritative start/host inventory')
            packet['empty_keeper_roster'] = True
        atomic_json(player['config']+'.ingestion-stop-permit.json',packet)


def final_visor_permits(work, records, observations, manifest, public_proofs):
    services = [retain_terminal(work, record, observations.get(record['config'], {}), manifest)
                for record in records if record['component'] in ('keeper','grapher','player')]
    for item in public_proofs:
        record = item['record']
        public = json.loads(item['proof_bytes'])
        packet = {'deployment_id':manifest['id'], 'stop_attempt':manifest['stop_attempt'],
                  'recipient':record, 'recipient_config_bytes':Path(record['config']).read_text(),
                  'recipient_native_generation':public['native_generation'],
                  'public_quiesced_bytes':item['proof_bytes'], 'services':services}
        atomic_json(record['config']+'.retirement-permit.json', packet)


def stop(work, hosts, budget_ms, worker=call_worker, now=time.monotonic, sleep=time.sleep):
    started = now()
    deadline = started+budget_ms/1000.0
    records = []
    lock = None
    try:
        lock_path = Path(work)/'monitor/lifecycle/control.lock'
        lock_path.parent.mkdir(parents=True, exist_ok=True)
        lock = lock_path.open('a+')
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        manifest_path = Path(work)/'monitor/lifecycle/deployment.json'
        manifest = json.loads(manifest_path.read_text())
        if manifest.get('state') not in ('running', 'stopping', 'starting', 'stopped') or not manifest.get('id'):
            raise RuntimeError('managed deployment identity is missing')
        if manifest.get('hosts') is not None and set(manifest['hosts']) != set(hosts):
            raise RuntimeError('managed authoritative host roster changed')
        if 'pending_launch' in manifest:
            raise RuntimeError('unverified original launch remains pending')
        if manifest['state'] not in ('stopping', 'stopped'):
            manifest.update(state='stopping', stop_attempt=str(uuid.uuid4()))
            atomic_json(manifest_path, manifest)
        for host in hosts:
            found = worker(host, work, 'inventory', deadline)
            for record in found['records']:
                record['_route'] = host
                records.append(record)
        expected = {(r['host'], r['config'], r['pid'], r['start_ticks'], r['boot_id'])
                    for r in manifest['records']}
        observed = {(r['host'], r['config'], r['pid'], r['start_ticks'], r['boot_id']) for r in records}
        if expected != observed:
            raise RuntimeError('managed deployment roster mismatch')
        manifest['stop_inventory_complete'] = True
        manifest['stop_inventory_routes'] = list(hosts)
        manifest['keeper_launches_closed'] = True
        atomic_json(manifest_path, manifest)
        observations = {}
        public_proofs = quiesce_visors(work, records, manifest, deadline, worker, now, sleep)
        query_proofs = quiesce_player_queries(work, records, manifest, deadline, worker, now, sleep)
        for phase in (('keeper',), ('player', 'grapher'), ('visor',)):
            if phase == ('player', 'grapher'):
                terminal_evidence(work, records, observations, manifest)
            if phase == ('visor',):
                final_visor_permits(work, records, observations, manifest, public_proofs)
            members = [record for record in records if record['component'] in phase]
            for record in members:
                if record['state'] == 'unknown':
                    raise RuntimeError('unverified process/completion identity')
                if record['state'] != 'stopped':
                    record['state'] = worker(record['_route'], work, 'term', deadline, record)['state']
            while members:
                pending = []
                for record in members:
                    observation = worker(record['_route'], work, 'inspect', deadline, record)
                    observations[record['config']] = observation
                    record['state'] = observation['state']
                    if record['state'] == 'unknown':
                        raise RuntimeError('process/SSH observation is unknown')
                    if record['state'] != 'stopped':
                        pending.append(record)
                members = pending
                if members:
                    if phase == ('keeper',):
                        progress_barrier(work, records, observations, manifest, deadline, worker, public_proofs, query_proofs)
                    remaining = deadline-now()
                    if remaining <= 0:
                        raise TimeoutError('aggregate stop budget expired')
                    sleep(min(.05, remaining))
        manifest['state'] = 'stopped'
        atomic_json(manifest_path, manifest)
        result = 0
        reason = 'all recorded processes completed orderly stop'
    except (OSError, ValueError, KeyError, RuntimeError, TimeoutError, subprocess.TimeoutExpired) as error:
        result = INCOMPLETE
        reason = str(error)
    if lock is not None:
        lock.close()
    elapsed = int((now()-started)*1000)
    for record in records:
        emit({key:record.get(key) for key in ('host','component','pid','start_ticks','config','state')}
             | {'operation':'stop', 'status':0 if record.get('state') == 'stopped' else INCOMPLETE,
                'elapsed_ms':elapsed})
    emit({'operation':'stop', 'aggregate':True, 'status':result, 'elapsed_ms':elapsed, 'reason':reason})
    return 0 if result == 0 else 27


def manifest_path(work):
    return Path(work)/'monitor/lifecycle/deployment.json'


def archive_previous(work, records):
    """Only after exact normal completion and authoritative absence of all rows."""
    root = Path(work)/'monitor/lifecycle'
    archive = root/'history'/str(uuid.uuid4())
    archive.mkdir(parents=True)
    for path in root.glob('*.json'):
        path.rename(archive/path.name)
    for record in records:
        for suffix in ('.lifecycle.json','.ready.json','.publisher-park.json','.park-ack.json',
                       '.public-quiesced.json','.retirement-permit.json','.stop-evidence.json',
                       '.queries-quiesce.json','.queries-quiesced.json','.ingestion-stop-permit.json'):
            path = Path(record['config']+suffix)
            if path.exists():
                key = hashlib.sha256(str(path).encode()).hexdigest()
                path.rename(archive/(key+suffix))


def fenced_command(work, hosts, budget_ms, operation, command, worker=call_worker, executables=()):
    started = time.monotonic()
    deadline = started+budget_ms/1000.0
    lock = None
    child = None
    try:
        root = Path(work)/'monitor/lifecycle'
        root.mkdir(parents=True, exist_ok=True)
        lock = (root/'control.lock').open('a+')
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        path = manifest_path(work)
        if path.exists():
            old = json.loads(path.read_text())
            if old.get('state') not in ('stopped','empty') or 'pending_launch' in old:
                raise RuntimeError('prior deployment is active or incomplete; configs are retained')
        records = []
        for host in hosts:
            found = worker(host, work, 'inventory', deadline,
                           {'executables':list(executables)} if executables else None)
            if any(record['state'] != 'stopped' for record in found['records']):
                raise RuntimeError('occupied/unverified deployment; configs are retained')
            records.extend(found['records'])
        archive_previous(work, records)
        deployment = str(uuid.uuid4())
        atomic_json(path, {'id':deployment,'state':'starting' if operation == 'start' else 'cleaning',
                           'records':[], 'keepers':[], 'hosts':hosts, 'inventory_complete':True})
        environment = dict(os.environ, CHRONOLOG_DEPLOYMENT_ID=deployment,
            CHRONOLOG_DEPLOYMENT_ROOT=str(work), CHRONOLOG_DEPLOYMENT_GUARD=str(lock.fileno()),
            CHRONOLOG_START_DEADLINE_NS=str(int(deadline*1000000000)))
        child = subprocess.Popen(command, env=environment, pass_fds=(lock.fileno(),))
        result = child.wait(timeout=max(0, deadline-time.monotonic()))
        if result != 0:
            raise RuntimeError('deployment command failed; recorded identities/configs are retained')
        manifest = json.loads(path.read_text())
        manifest['state'] = 'running' if operation == 'start' else 'empty'
        atomic_json(path, manifest)
        emit({'operation':operation, 'status':0, 'deployment_id':deployment,
              'elapsed_ms':int((time.monotonic()-started)*1000)})
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        # Only the exact shell helper is terminated on caller expiry. Services
        # retain their identities and dependencies; no service signal is sent.
        if child is not None and child.poll() is None:
            child.kill()
        emit({'operation':operation, 'status':INCOMPLETE,
              'elapsed_ms':int((time.monotonic()-started)*1000), 'reason':str(error)})
        return 27
    finally:
        if lock is not None:
            lock.close()


def validate_guard(work):
    descriptor = int(os.environ.get('CHRONOLOG_DEPLOYMENT_GUARD', '-1'))
    expected = Path(work)/'monitor/lifecycle/control.lock'
    held, actual = os.fstat(descriptor), expected.stat()
    if (held.st_dev, held.st_ino) != (actual.st_dev, actual.st_ino):
        raise RuntimeError('deployment mutation has no inherited fence')
    with expected.open('a+') as other:
        try:
            fcntl.flock(other.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            raise RuntimeError('deployment fence is not held')
    manifest = json.loads(manifest_path(work).read_text())
    if (manifest.get('id') != os.environ.get('CHRONOLOG_DEPLOYMENT_ID')
        or manifest.get('state') not in ('starting','cleaning')
        or str(work) != os.environ.get('CHRONOLOG_DEPLOYMENT_ROOT')):
        raise RuntimeError('deployment mutation identity/fence mismatch')


def launch(work, specification):
    """Launch once and retain the exact identity before awaiting Running."""
    started = time.monotonic()
    deadline = started+specification['budget_ms']/1000.0
    deployment = specification['deployment_id']
    manifest = json.loads(manifest_path(work).read_text())
    if manifest.get('state') != 'starting' or manifest.get('id') != deployment:
        raise RuntimeError('launch is outside its fenced deployment')
    component = specification['component']
    executable = str(Path(specification['executable']).resolve())
    config = str(Path(specification['config'].replace('{hostname}', socket.gethostname())).resolve())
    output = Path(specification['output'].replace('{hostname}', socket.gethostname()))
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, CHRONOLOG_DEPLOYMENT_ID=deployment,
                       CHRONOLOG_DEPLOYMENT_ROOT=str(work))
    pending = manifest.get('pending_launch',{})
    if pending.get('specification') != specification or not specification.get('launch_id'):
        raise RuntimeError('original launch reservation is missing or changed')
    with output.open('w') as log:
        child = subprocess.Popen([executable, '--config', config], env=environment,
                                 stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                 start_new_session=True, close_fds=True)
    record = None
    original = None
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError('launched service exited during startup; original launch retained')
        # The exec-error handshake can precede observable argument setup. Only
        # empty arguments on this exact original lifetime are re-observed; no
        # replacement launch or new startup budget is created.
        if record is None:
            try:
                record = record_process(work, child.pid, executable, config, component, original)
            except ExecArgumentsPending as pending:
                if original is None:
                    original = pending.observed
                time.sleep(min(.02, max(0, deadline-time.monotonic())))
                continue
            manifest = json.loads(manifest_path(work).read_text())
            if manifest.get('state') != 'starting' or manifest.get('id') != deployment:
                raise RuntimeError('deployment fence changed during launch')
            if manifest.get('pending_launch',{}).get('specification') != specification:
                raise RuntimeError('original launch reservation changed before identity record')
            manifest.pop('pending_launch')
            manifest['records'].append(record)
            if component == 'keeper':
                manifest['keepers'].append(record)
            atomic_json(manifest_path(work), manifest)
        if child.poll() is not None:
            raise RuntimeError('launched service exited during startup; identity retained')
        try:
            current = json.loads(Path(record['status_file']).read_text())
            if (same_identity(current, record) and current.get('deployment_id') == deployment
                and current.get('state') == 'Running' and current.get('status') == 0
                and matching(process_identity(child.pid), record)):
                return record
        except (OSError, ValueError):
            pass
        time.sleep(min(.02, max(0, deadline-time.monotonic())))
    raise TimeoutError('startup budget expired; original launched service is retained')


def launch_remote(work, host, executable, config, component, output, budget_ms):
    specification = dict(executable=executable, config=config, component=component,
                         output=output, budget_ms=budget_ms,
                         deployment_id=os.environ.get('CHRONOLOG_DEPLOYMENT_ID',''), launch_id=str(uuid.uuid4()))
    validate_guard(work)
    manifest = json.loads(manifest_path(work).read_text())
    if manifest.get('state') != 'starting' or manifest.get('id') != specification['deployment_id'] or 'pending_launch' in manifest:
        raise RuntimeError('another launch remains pending or deployment start is closed')
    # Retain intent BEFORE SSH/helper dispatch. A failed SSH cannot hide a
    # delayed remote launch and turn an empty observation into a safe permit.
    manifest['pending_launch'] = {'route':host, 'specification':specification}
    atomic_json(manifest_path(work), manifest)
    return call_worker(host, work, 'launch', time.monotonic()+budget_ms/1000.0, specification)


def bounded_stop(work, hosts, budget_ms):
    # Bound even shared-filesystem observations in the controller child. The
    # exact overrun controller may be killed; recorded services never are.
    started = time.monotonic()
    command = [sys.executable, str(Path(__file__).resolve()), '--work-dir', str(work),
               '--budget-ms', str(budget_ms)]
    for host in hosts:
        command += ['--host', host]
    command += ['internal-stop']
    child = subprocess.Popen(command)
    try:
        return child.wait(timeout=max(0, budget_ms/1000.0-(time.monotonic()-started)))
    except subprocess.TimeoutExpired:
        child.kill()
        emit({'operation':'stop','aggregate':True,'status':INCOMPLETE,
              'elapsed_ms':int((time.monotonic()-started)*1000),
              'reason':'aggregate stop budget expired; original services retained'})
        return 27


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--work-dir', required=True)
    parser.add_argument('--host', action='append', default=[])
    parser.add_argument('--occupied-executable', action='append', default=[])
    parser.add_argument('--budget-ms', type=int, default=30000)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('stop')
    sub.add_parser('internal-stop')
    sub.add_parser('guard')
    for name in ('run-start','run-clean'):
        action = sub.add_parser(name)
        action.add_argument('arguments', nargs=argparse.REMAINDER)
    launching = sub.add_parser('launch')
    launching.add_argument('--executable', required=True)
    launching.add_argument('--config', required=True)
    launching.add_argument('--component', choices=COMPONENTS, required=True)
    launching.add_argument('--output', required=True)
    sub.add_parser('verify-empty')
    record = sub.add_parser('record')
    record.add_argument('--pid', type=int, required=True)
    record.add_argument('--executable', required=True)
    record.add_argument('--config', required=True)
    record.add_argument('--component', choices=COMPONENTS, required=True)
    worker = sub.add_parser('worker')
    worker.add_argument('action', choices=('inventory','term','inspect','launch'))
    worker.add_argument('--record')
    args = parser.parse_args()
    if args.budget_ms <= 0:
        parser.error('budget must be positive')
    work = Path(args.work_dir).resolve()
    if args.command == 'record':
        emit(record_process(work, args.pid, args.executable, args.config, args.component))
    elif args.command == 'worker':
        if args.action == 'inventory':
            emit(inventory(work, json.loads(args.record).get('executables',[]) if args.record else ()))
        else:
            record = json.loads(args.record)
            if args.action == 'launch':
                os.environ['CHRONOLOG_DEPLOYMENT_ID'] = record['deployment_id']
                emit(launch(work, record))
            else:
                emit({'state':signal_process(record)} if args.action == 'term' else inspect_process(record))
    elif args.command == 'stop':
        return bounded_stop(work, args.host or ['local'], args.budget_ms)
    elif args.command == 'internal-stop':
        return stop(work, args.host or ['local'], args.budget_ms)
    elif args.command in ('run-start','run-clean'):
        command = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
        if not command:
            parser.error('deployment command is required')
        return fenced_command(work, args.host or ['local'], args.budget_ms,
                              'start' if args.command == 'run-start' else 'clean', command, executables=args.occupied_executable)
    elif args.command == 'guard':
        validate_guard(work)
    elif args.command == 'launch':
        if 'CHRONOLOG_START_DEADLINE_NS' in os.environ:
            args.budget_ms = min(args.budget_ms, max(0, (int(os.environ['CHRONOLOG_START_DEADLINE_NS'])-time.monotonic_ns())//1000000))
        if args.budget_ms <= 0:
            raise TimeoutError('original startup budget expired before launch')
        launched = launch_remote(work, (args.host or ['local'])[0], args.executable, args.config,
                                 args.component, args.output, args.budget_ms)
        emit({key:launched.get(key) for key in ('host','component','pid','start_ticks','config','deployment_id')}
             | {'operation':'launch','status':0,'state':'Running'})
    else:
        deadline = time.monotonic()+args.budget_ms/1000.0
        for host in args.host or ['local']:
            found = call_worker(host, work, 'inventory', deadline)
            if any(record['state'] != 'stopped' for record in found['records']):
                raise RuntimeError('occupied/unverified deployment; configs are retained')
        emit({'operation':'verify-empty', 'status':0})
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError, RuntimeError, TimeoutError, subprocess.TimeoutExpired) as error:
        emit({'status':INCOMPLETE, 'reason':str(error)})
        sys.exit(27)
