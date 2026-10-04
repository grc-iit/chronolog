import ctypes
import json
import os
from pathlib import Path
import queue
import secrets
import threading
import time

MARKER = '.chronolog-tier.json'
IDENTITY = ('deployment_id', 'name', 'rank', 'kind', 'tier_uuid', 'f_type')
# The Grapher's tier view (its tier_status_file), relative to the instance folder, which is every service's working
# directory. It lives with the supervisor's own status, never under a tier root (I13.15).
GRAPHER_STATUS = 'run/grapher-tiers.json'
GRAPHER_STATUS_LIMIT = 1 << 20


class StatFS(ctypes.Structure):
    _fields_ = [('f_type', ctypes.c_long), ('f_bsize', ctypes.c_long),
                ('f_blocks', ctypes.c_ulong), ('f_bfree', ctypes.c_ulong),
                ('f_bavail', ctypes.c_ulong), ('f_files', ctypes.c_ulong),
                ('f_ffree', ctypes.c_ulong), ('f_fsid', ctypes.c_int * 2),
                ('f_namelen', ctypes.c_long), ('f_frsize', ctypes.c_long),
                ('f_flags', ctypes.c_long), ('f_spare', ctypes.c_long * 4)]


def filesystem(fd):
    value = StatFS()
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.fstatfs(fd, ctypes.byref(value)) != 0:
        raise OSError(ctypes.get_errno(), 'fstatfs failed')
    return {'f_type': value.f_type, 'st_dev': os.fstat(fd).st_dev,
            'f_fsid': list(value.f_fsid)}


def read_marker(fd):
    marker_fd = os.open(MARKER, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=fd)
    with os.fdopen(marker_fd) as source:
        value = json.load(source)
    if not isinstance(value, dict) or any(key not in value for key in IDENTITY + ('st_dev', 'f_fsid')):
        raise ValueError('invalid tier marker')
    uuid = value['tier_uuid']
    if not isinstance(uuid, str) or len(uuid) != 32 or any(c not in '0123456789abcdef' for c in uuid):
        raise ValueError('invalid tier uuid')
    return value


def validate_marker(marker, expected, actual):
    if marker.get('f_type') != actual['f_type']:
        raise ValueError('tier root has another file system type')
    if any(marker.get(key) != expected.get(key) for key in IDENTITY):
        raise ValueError('tier marker identity mismatch')


def add_marker(tier, deployment_id):
    root = Path(tier['root'])
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        actual = filesystem(fd)
        expected = dict(tier, deployment_id=deployment_id)
        try:
            marker = read_marker(fd)
        except FileNotFoundError:
            marker = dict(actual, deployment_id=deployment_id, name=tier['name'],
                          rank=tier['rank'], kind=tier['kind'], tier_uuid=secrets.token_hex(16))
            # Explicit creation never replaces a marker, even under a concurrent tier-add.
            marker_fd = os.open(MARKER, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                                0o600, dir_fd=fd)
            with os.fdopen(marker_fd, 'w') as target:
                json.dump(marker, target)
                target.write('\n')
                target.flush()
                os.fsync(target.fileno())
            os.fsync(fd)
        else:
            if marker.get('f_type') != actual['f_type']:
                raise ValueError('tier root has another file system type')
            expected.setdefault('tier_uuid', marker.get('tier_uuid'))
            expected.setdefault('f_type', marker.get('f_type'))
            validate_marker(marker, expected, actual)
        return dict(tier, **{key: marker[key] for key in ('tier_uuid', 'f_type', 'st_dev', 'f_fsid')})
    finally:
        os.close(fd)


def initialize_local(record):
    record.setdefault('deployment_id', record['id'])
    local = dict(record['tiers'][0], rank=0)
    record['tiers'][0] = add_marker(local, record['deployment_id'])


def has_tier_table(record):
    return 'deployment_id' in record and all('tier_uuid' in tier for tier in record['tiers'])


def tier_config_keys(record):
    return {'deployment_id': record['deployment_id'],
            'tiers': [{key: tier[key] for key in ('name', 'kind', 'root', 'rank', 'tier_uuid',
                                                 'f_type', 'st_dev', 'f_fsid')}
                      for tier in record['tiers']]}


def probe_tier(tier, deployment_id):
    fd = os.open(tier['root'], os.O_RDONLY | os.O_DIRECTORY)
    try:
        actual = filesystem(fd)
        if deployment_id is not None:
            validate_marker(read_marker(fd), dict(tier, deployment_id=deployment_id), actual)
        fs = os.fstatvfs(fd)
        return dict(actual, available=os.access('.', os.W_OK, dir_fd=fd),
                    used_bytes=(fs.f_blocks - fs.f_bfree) * fs.f_frsize)
    finally:
        os.close(fd)


def probe_worker(operation, tier, deployment_id, replies):
    try:
        result = operation(tier, deployment_id)
    except (OSError, ValueError) as error:
        result = {'available': False, 'used_bytes': None, 'probe_error': str(error)}
    replies.put((time.monotonic(), result))


class TierProbe:
    def __init__(self, tier, deployment_id, timeout_ms, interval_ms, operation=probe_tier):
        self.tier = dict(tier)
        self.deployment_id = deployment_id
        self.timeout = timeout_ms / 1000
        self.interval = interval_ms / 1000
        self.operation = operation
        self.replies = queue.SimpleQueue()
        self.thread = None
        self.deadline = 0
        self.next_probe = 0
        self.result = {'available': False, 'used_bytes': None}

    def poll(self):
        now = time.monotonic()
        if self.thread is not None:
            try:
                finished, result = self.replies.get_nowait()
            except queue.Empty:
                if now >= self.deadline:
                    self.result = {'available': False, 'used_bytes': None,
                                   'probe_error': 'tier I/O deadline'}
            else:
                self.result = result if finished <= self.deadline else {
                    'available': False, 'used_bytes': None, 'probe_error': 'tier I/O deadline'}
                self.thread = None
                self.next_probe = now + self.interval
        if self.thread is None and now >= self.next_probe:
            self.result = {'available': False, 'used_bytes': None}
            self.deadline = now + self.timeout
            self.thread = threading.Thread(target=probe_worker, daemon=True,
                args=(self.operation, self.tier, self.deployment_id, self.replies))
            self.thread.start()
        return dict(self.result)


def read_grapher_status(path):
    """The Grapher's status file as (status, None), or (None, reason). The file carries no time: the Grapher rewrites
    it only when its content changes, so the caller decides whether a live Grapher stands behind it."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return None, 'absent'
    except OSError:
        return None, 'unreadable'
    try:
        with os.fdopen(fd, 'rb') as source:
            data = source.read(GRAPHER_STATUS_LIMIT + 1)
        value = json.loads(data) if len(data) <= GRAPHER_STATUS_LIMIT else None
    except (OSError, ValueError):
        return None, 'unparsable'
    shape = {'name': str, 'rank': int, 'available': bool, 'used_bytes': int, 'budget_bytes': int, 'above_high': bool}
    if not (isinstance(value, dict) and isinstance(value.get('tiers'), list) and
            isinstance(value.get('writer'), str) and
            type(value.get('migrate_enabled')) is bool and type(value.get('migration_stopped')) is bool and
            isinstance(value.get('pending_tier_deletions'), dict) and
            all(isinstance(tier, dict) and all(type(tier.get(key)) is kind for key, kind in shape.items())
                for tier in value['tiers'])):
        return None, 'unparsable'
    return value, None


def grapher_views(folder, record, running):
    """(per-tier views in the order of record['tiers'], migration view). Anything short of a well-formed file
    written by the running Grapher is reported as unknown, never as available."""
    if not has_tier_table(record):
        status, reason = None, 'no tier table'
    elif not running:
        status, reason = None, 'grapher not running'
    else:
        status, reason = read_grapher_status(Path(folder) / GRAPHER_STATUS)
    if status is None:
        unknown = {'known': False, 'reason': reason}
        return [dict(unknown) for _ in record['tiers']], dict(unknown)
    seen = {tier['name']: tier for tier in status['tiers']}
    views = []
    for tier in record['tiers']:
        entry = seen.get(tier['name'])
        if entry is None or entry['rank'] != tier.get('rank', 0):
            # A tier added after the Grapher read its configuration.
            views.append({'known': False, 'reason': 'not in the Grapher tier table'})
        else:
            views.append({'known': True, 'available': entry['available'], 'used_bytes': entry['used_bytes'],
                          'budget_bytes': entry['budget_bytes'], 'above_high': entry['above_high'],
                          'migrate_enabled': status['migrate_enabled'],
                          'migration_stopped': status['migration_stopped']})
    return views, {'known': True, 'writer': status['writer'], 'migrate_enabled': status['migrate_enabled'],
                   'migration_stopped': status['migration_stopped'],
                   'pending_tier_deletions': status['pending_tier_deletions']}
