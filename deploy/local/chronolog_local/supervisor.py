import ctypes
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import time

from .registry import CONTROL_LOCKS, ROLES, atomic, binary, boot_id, clock_status, control, free_ports, leases, load, lock, probe

from .tiers import TierProbe, probe_tier

READY = {'visor': 'catalog ready', 'keeper': 'journal ready',
         'grapher': 'grapher registered', 'player': 'player ready'}
LOG_LIMIT = 8 << 20


def configs(record):
    e, p = record['endpoints'], record['paths']
    common = {'visor_internal': e['catalog_internal']}
    result = {
        'visor': {'listen': e['catalog'], 'internal_listen': e['catalog_internal'],
                  'db_path': p['catalog_db'], 'keepers': [{'process_id': 'keeper-1', 'endpoint': e['keeper']}],
                  'grapher': e['grapher'], 'player': e['player']},
        'keeper': dict(common, listen=e['keeper'], internal_listen=e['keeper_internal'],
                       process_id='keeper-1', self_endpoint=e['keeper'], wal_dir=p['wal_dir']),
        'grapher': dict(common, internal_listen=e['grapher'], self_endpoint=e['grapher'],
                        process_id='grapher-1', manifest_writer='grapher-1', archive_root=record['tiers'][0]['root']),
        'player': dict(common, listen=e['player'], advertise=e['player'], visor=e['catalog'],
                       keeper_internal={'keeper-1': e['keeper_internal']}, archive_root=record['tiers'][0]['root'])}
    defaults = {role: dict(config) for role, config in result.items()}
    for role, config in result.items():
        config.update(record['overrides'].get(role, {}))
        for key in ('listen', 'internal_listen', 'advertise', 'self_endpoint'):
            if key in config and config[key].split(':')[0] != '127.0.0.1' and not record.get('insecure_bind_all'):
                raise ValueError(f'{role}.{key} requires --insecure-bind-all')
        for key, value in defaults[role].items():
            if config[key] != value:
                raise ValueError(f'{role}.{key} is fixed by instance.json')
    return result


class Supervisor:
    def __init__(self, folder, record, tier_probe_operation=probe_tier):
        self.folder, self.record = folder, record
        self.tier_probe_operation = tier_probe_operation
        self.tier_probes = {}
        self.remounts = set()
        self.children = {}
        self.offsets = {}
        self.exits = {role: [] for role in ROLES}
        self.retry_at = {}
        self.stopping = False
        self.idle_since = None
        self.state = {'state': 'starting', 'supervisor_pid': os.getpid(), 'boot_id': boot_id(),
                      'started_ns': time.time_ns(), 'ready_ns': 0, 'services': {}, 'attach': {},
                      'tiers': [], 'clock': {'status': 'Unavailable', 'uncertainty_ns': None}, 'probe': 'tcp'}
        signal.signal(signal.SIGTERM, self.request_stop)
        signal.signal(signal.SIGINT, self.request_stop)

    def log(self, message):
        path = self.folder / 'logs/supervisor.log'
        if path.exists() and path.stat().st_size > LOG_LIMIT:
            os.replace(path, path.with_suffix('.log.1'))
        with path.open('a') as out:
            out.write(json.dumps({'time_ns': time.time_ns(), 'message': message}) + '\n')

    def request_stop(self, *_):
        self.stopping = True

    def publish(self):
        holders = leases(self.folder)
        self.state['attach'] = {'count': len(holders), 'holders': holders}
        self.state['updated_ns'] = time.time_ns()
        self.state['clock'] = clock_status()
        for role in self.children:
            path = self.folder / 'logs' / (role + '.log')
            if path.stat().st_size > LOG_LIMIT:
                with path.open('rb') as source, path.with_suffix('.log.1').open('wb') as target:
                    source.seek(-LOG_LIMIT, os.SEEK_END)
                    target.write(source.read(LOG_LIMIT))
                with path.open('r+b') as log:
                    log.truncate(0)
                self.offsets[role] = 0
        latest = load(self.folder / 'instance.json')
        self.record['tiers'] = latest['tiers']
        if 'deployment_id' in latest:
            self.record['deployment_id'] = latest['deployment_id']
        self.state['tiers'] = []
        for tier in self.record['tiers']:
            key = (tier['name'], tier.get('tier_uuid'))
            if key not in self.tier_probes:
                self.tier_probes[key] = TierProbe(tier, self.record.get('deployment_id'),
                    self.record.get('tier_io_timeout_ms', 1000),
                    self.record.get('tier_probe_interval_ms', 5000), self.tier_probe_operation)
            result = self.tier_probes[key].poll()
            remount = (tier['name'], result.get('st_dev'), tuple(result.get('f_fsid', [])))
            if result.get('available') and tier.get('tier_uuid') and (
                    result['st_dev'] != tier['st_dev'] or result['f_fsid'] != tier['f_fsid']
                    ) and remount not in self.remounts:
                self.log('tier remount ' + json.dumps(dict(name=tier['name'],
                         st_dev=result['st_dev'], f_fsid=result['f_fsid'])))
                self.remounts.add(remount)
            self.state['tiers'].append(dict(tier, **result,
                usage_scope='filesystem', budget_bytes=tier.get('budget_bytes', 0)))
        atomic(self.folder / 'run/status.json', self.state)
        return holders

    def start(self, role):
        path = self.folder / 'logs' / (role + '.log')
        if path.exists() and path.stat().st_size > LOG_LIMIT:
            os.replace(path, path.with_suffix('.log.1'))
        out = path.open('ab')
        self.offsets[role] = out.tell()
        parent = os.getpid()

        def death_signal():
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
                os._exit(125)
            if os.getppid() != parent:
                os.kill(os.getpid(), signal.SIGKILL)

        command = [binary(self.record['bin_dir'], role), '--config',
                   str(self.folder / 'config' / (role + '.json'))]
        if self.record.get('insecure_bind_all') and role != 'player':
            command.append('--insecure-bind-all')
        # Service environment overrides cannot bypass the descriptor's bind guard.
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(('CHRONOLOG_VISOR_', 'CHRONOLOG_KEEPER_',
                                      'CHRONOLOG_GRAPHER_', 'CHRONOLOG_PLAYER_'))}
        try:
            child = subprocess.Popen(command, cwd=self.folder, stdout=out, stderr=subprocess.STDOUT,
                                     env=env, preexec_fn=death_signal)
        finally:
            out.close()
        self.children[role] = child
        previous = self.state['services'].get(role, {})
        self.state['services'][role] = {'pid': child.pid, 'state': 'starting', 'ready_ns': 0,
                                        'restarts': previous.get('restarts', -1) + 1}
        self.log('start ' + role)

    def ready(self, role):
        child = self.children[role]
        if child.poll() is not None:
            return False
        if self.state['services'][role]['state'] == 'ready':
            return True
        with (self.folder / 'logs' / (role + '.log')).open('rb') as log:
            log.seek(self.offsets[role])
            found = READY[role].encode() in log.read()
        if found and not self.state['services'][role]['ready_ns']:
            self.state['services'][role].update(state='ready', ready_ns=time.time_ns())
            self.log('ready ' + role)
        return found

    def wait_ready(self, roles, deadline):
        while time.monotonic() < deadline and not self.stopping:
            if all(self.ready(role) for role in roles):
                self.publish()
                return
            for role in roles:
                if self.children[role].poll() is not None:
                    raise RuntimeError(f'{role} exited before readiness')
            self.publish()
            time.sleep(0.05)
        raise RuntimeError('boot deadline or stop requested while waiting for ' + ','.join(roles))

    def stop(self):
        self.state['state'] = 'stopping'
        self.publish()
        for role in ('player', 'keeper', 'grapher', 'visor'):
            child = self.children.get(role)
            if not child or child.poll() is not None:
                continue
            self.log('stop ' + role)
            child.terminate()
            timeout = 10
            if role == 'keeper':
                timeout = configs(self.record)['keeper'].get('shutdown_confirm_timeout_secs', 150) + 5
            elif role == 'grapher':
                timeout = configs(self.record)['grapher'].get('drain_timeout_ms', 5000) / 1000 + 5
            end = time.monotonic() + timeout
            while child.poll() is None and time.monotonic() < end:
                self.publish()
                time.sleep(0.05)
            if child.poll() is None:
                self.log('kill deadline ' + role)
                child.kill()
            child.wait(timeout=5)
            self.state['services'][role]['state'] = 'stopped'
        self.publish()

    def run(self):
        try:
            self.publish()
            config = configs(self.record)
            for role in ROLES:
                binary(self.record['bin_dir'], role)
                atomic(self.folder / 'config' / (role + '.json'), config[role])
            for path in (self.record['paths']['wal_dir'], self.record['tiers'][0]['root']):
                if not Path(path).is_dir():
                    raise ValueError('missing local data directory: ' + path)
            free_ports(self.record['endpoints'])
            deadline = time.monotonic() + 30
            self.start('visor')
            self.wait_ready(('visor',), deadline)
            self.start('keeper')
            self.start('grapher')
            self.wait_ready(('keeper', 'grapher'), deadline)
            self.start('player')
            self.wait_ready(('player',), deadline)
            self.state['probe'] = probe(self.record)
            for role in ROLES:
                if self.children[role].poll() is not None:
                    raise RuntimeError(f'{role} exited during readiness probe')
            with control(self.folder / 'run/control.lock'):
                self.record = load(self.folder / 'instance.json')
                self.record['booted'] = True
                atomic(self.folder / 'instance.json', self.record)
            self.state.update(state='ready', ready_ns=time.time_ns())
            self.publish()
            while not self.stopping:
                now = time.monotonic()
                for role, child in list(self.children.items()):
                    if child.poll() is not None and role not in self.retry_at:
                        self.exits[role] = [t for t in self.exits[role] if now - t < 60] + [now]
                        self.state['services'][role]['state'] = 'failed'
                        self.retry_at[role] = now + min(30, 0.5 * 2**(len(self.exits[role]) - 1))
                        self.log('exit ' + role + ' ' + str(child.returncode))
                    if role in self.retry_at and now >= self.retry_at[role] and len(self.exits[role]) < 5:
                        self.start(role)
                        del self.retry_at[role]
                    if self.children[role].poll() is None:
                        self.ready(role)
                self.state['state'] = 'ready' if all(
                    s['state'] == 'ready' for s in self.state['services'].values()) else 'degraded'
                holders = self.publish()
                if holders:
                    self.idle_since = None
                elif self.idle_since is None:
                    self.idle_since = now
                if self.record['policy']['on_last_detach'] == 'stop' and not holders and (
                        now - self.idle_since >= self.record['policy']['idle_grace_s']):
                    fd = os.open(self.folder / 'run/control.lock', os.O_RDWR | os.O_CREAT, 0o600)
                    try:
                        fcntl.flock(fd, fcntl.LOCK_EX)
                        if not leases(self.folder):
                            self.state['state'] = 'stopping'
                            self.publish()
                            self.stopping = True
                    finally:
                        os.close(fd)
                time.sleep(0.2)
        except Exception as error:
            self.state.update(state='degraded', error=str(error))
            self.log(str(error))
        finally:
            self.stop()


def detach(folder):
    # Double fork leaves the supervisor outside the invoking harness's process group.
    first = os.fork()
    if first:
        os.waitpid(first, 0)
        return
    os.setsid()
    if os.fork():
        os._exit(0)
    devnull = os.open(os.devnull, os.O_RDWR)
    for fd in (0, 1, 2):
        os.dup2(devnull, fd)
    os.close(devnull)
    # The daemon must not retain the caller's control lock or output pipes.
    for inherited in list(Path('/proc/self/fd').iterdir()):
        if int(inherited.name) > 2:
            try:
                os.close(int(inherited.name))
            except OSError:
                pass
    CONTROL_LOCKS.clear()
    with control(folder / 'run/control.lock'):
        fd = lock(folder / 'run/supervisor.lock')
        if fd is None:
            os._exit(0)
    try:
        Supervisor(folder, load(folder / 'instance.json')).run()
    finally:
        os.close(fd)
    os._exit(0)
