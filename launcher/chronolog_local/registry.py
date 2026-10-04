import ctypes
import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import socket
import shutil
import sys
import time

ROLES = ('visor', 'keeper', 'grapher', 'player')
KEYS = ('catalog', 'catalog_internal', 'keeper', 'keeper_internal', 'grapher', 'player')


def directory(path):
    path = Path(path)
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if path.stat().st_uid != os.getuid():
        raise ValueError(f'not owned by this uid: {path}')
    os.chmod(path, 0o700)
    return path


def home():
    root = directory(Path(os.environ.get('CHRONOLOG_HOME', str(Path(os.environ.get(
        'XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'chronolog'))).resolve())
    buf = ctypes.create_string_buffer(256)
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.statfs(os.fsencode(root), buf) != 0:
        raise OSError(ctypes.get_errno(), 'statfs failed')
    if ctypes.c_long.from_buffer(buf).value == 0x6969:
        raise ValueError('CHRONOLOG_HOME must not be on NFS; liveness requires local flock')
    for name in ('instances', 'external', 'locks'):
        directory(root / name)
    return root


def name_check(name):
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,31}', name):
        raise ValueError('name must match [a-z0-9][a-z0-9-]{0,31}')
    return name


def atomic(path, value):
    path = Path(path)
    tmp = path.with_name(path.name + '.' + secrets.token_hex(8))
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as out:
            json.dump(value, out)
            out.write('\n')
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, path)
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        tmp.unlink(missing_ok=True)


def load(path):
    return json.loads(Path(path).read_text())


def lock(path, exclusive=True):
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        return None
    return fd


def leases(folder):
    result = []
    for path in sorted((folder / 'run/attach').glob('*.json')):
        try:
            fd = lock(path)
            if fd is None:
                result.append(dict(load(path), lease=path.stem))
            else:
                path.unlink(missing_ok=True)
                os.close(fd)
        except (FileNotFoundError, json.JSONDecodeError):
            pass
    return result


def boot_id():
    return Path('/proc/sys/kernel/random/boot_id').read_text().strip()


def probe(record):
    endpoints = record['endpoints']
    try:
        import chronolog
    except ImportError:
        for key in ('catalog', 'player'):
            if key in endpoints:
                last = None
                for endpoint in endpoints[key].split(','):
                    host, port = endpoint.removeprefix('dns:///').rsplit(':', 1)
                    try:
                        with socket.create_connection((host, int(port)), timeout=1):
                            pass
                        last = None
                        break
                    except OSError as error:
                        last = error
                if last:
                    raise last
        return 'tcp'
    client = chronolog.connect(endpoints['catalog'], player=endpoints.get('player'), timeout=1, max_retries=0)
    try:
        client.list_chronicles(timeout=1)
        try:
            list(client.read(2**64 - 1, timeout=1))
        except (chronolog.NotFound, chronolog.FailedPrecondition):
            pass
    finally:
        del client
    return 'rpc'


CONTROL_LOCKS = {}


@contextlib.contextmanager
def control(path):
    key = str(Path(path).resolve())
    if key in CONTROL_LOCKS:
        yield
        return
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        CONTROL_LOCKS[key] = fd
        yield
    finally:
        CONTROL_LOCKS.pop(key, None)
        os.close(fd)


def status(folder, probing=False):
    # Serialize the brief discovery SH lock with a supervisor's first EX acquisition.
    with control(folder / 'run/control.lock'):
        return locked_status(folder, probing)


def locked_status(folder, probing=False):
    record = load(folder / 'instance.json')
    fd = lock(folder / 'run/supervisor.lock', exclusive=False)
    if fd is not None:
        os.close(fd)
        return dict(record, state='stopped', attach={'count': 0, 'holders': []})
    try:
        current = load(folder / 'run/status.json')
    except FileNotFoundError:
        current = {}
    if current.get('boot_id') != boot_id():
        current['state'] = 'starting'
    elif time.time_ns() - current.get('updated_ns', 0) > 10_000_000_000:
        current['state'] = 'unresponsive'
    if current.get('state') in ('starting', 'unresponsive'):
        # No live supervisor stands behind the published Grapher tier view.
        unknown = {'known': False, 'reason': 'supervisor status is ' + current['state']}
        if 'tier_migration' in current:
            current['tier_migration'] = dict(unknown)
        current['tiers'] = [dict(tier, grapher=dict(unknown)) if 'grapher' in tier else tier
                            for tier in current.get('tiers', record['tiers'])]
    if probing:
        try:
            current['probe'] = probe(record)
        except Exception as error:
            current.update(state='degraded', probe_error=str(error))
    return dict(record, **{key: value for key, value in current.items() if key not in ('tiers',)},
                tiers=current.get('tiers', record['tiers']))


def find(name):
    root = home()
    direct = root / 'instances' / name_check(name)
    if (direct / 'instance.json').exists():
        return direct, load(direct / 'instance.json')
    external = root / 'external' / (name + '.json')
    if external.exists():
        return None, load(external)
    for folder in (root / 'instances').iterdir():
        if (folder / 'instance.json').exists():
            record = load(folder / 'instance.json')
            if record['id'] == name:
                return folder, record
    for path in (root / 'external').glob('*.json'):
        record = load(path)
        if record['id'] == name:
            return None, record
    raise ValueError(f'no instance {name}; start one with: chronolog up {name} --bin-dir <directory>')


def process_gone(pid):
    process = Path('/proc') / str(pid)
    try:
        zombie = (process / 'stat').read_text().rsplit(')', 1)[1].split()[0] == 'Z'
        return zombie and {task.name for task in (process / 'task').iterdir()} == {str(pid)}
    except (FileNotFoundError, ProcessLookupError):
        return not process.exists()


def port_holder(port):
    inodes = set()
    for table in ('tcp', 'tcp6'):
        for line in Path('/proc/net/' + table).read_text().splitlines()[1:]:
            fields = line.split()
            if int(fields[1].rsplit(':', 1)[1], 16) == port:
                inodes.add(fields[9])
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            for task in (process / 'task').iterdir():
                try:
                    for descriptor in (task / 'fd').iterdir():
                        if os.readlink(descriptor) in {'socket:[' + inode + ']' for inode in inodes}:
                            return 'pid=' + process.name + ' name=' + (task / 'comm').read_text().strip()
                except (PermissionError, FileNotFoundError, ProcessLookupError):
                    continue
        except (PermissionError, FileNotFoundError, ProcessLookupError):
            pass
    return 'holder unavailable (kernel socket or another uid)'


def clock_status():
    class TimexPrefix(ctypes.Structure):
        _fields_ = [('modes', ctypes.c_uint), ('offset', ctypes.c_long), ('freq', ctypes.c_long),
                    ('maxerror', ctypes.c_long), ('esterror', ctypes.c_long), ('status', ctypes.c_int)]
    value = ctypes.create_string_buffer(256)
    result = ctypes.CDLL(None, use_errno=True).ntp_adjtime(value)
    fields = TimexPrefix.from_buffer(value)
    synced = result >= 0 and not fields.status & 0x40 and 0 <= fields.maxerror <= 1_000_000
    return {'status': 'Synced' if synced else ('Unsynced' if result >= 0 else 'Unavailable'),
            'uncertainty_ns': fields.maxerror * 1000 if synced else None}


def free_ports(endpoints):
    sockets = []
    try:
        for address in endpoints.values():
            host, port = address.rsplit(':', 1)
            sock = socket.socket()
            sockets.append(sock)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                sock.bind((host, int(port)))
            except OSError as error:
                raise ValueError(f'cannot bind fixed endpoint {address}: {error}; {port_holder(int(port))}') from error
    finally:
        for sock in sockets:
            sock.close()


def binary(bin_dir, role):
    selected = bin_dir or os.environ.get('CHRONOLOG_BIN_DIR')
    if not selected:
        beside = Path(sys.argv[0]).resolve().parent / ('chrono_' + role)
        if beside.is_file() and os.access(beside, os.X_OK):
            return str(beside)
        found = shutil.which('chrono_' + role)
        if found:
            return str(Path(found).resolve())
        raise ValueError(f'cannot find chrono_{role}; set --bin-dir or CHRONOLOG_BIN_DIR, install beside chronolog, or add to PATH')
    root = Path(selected)
    candidates = (root / ('chrono_' + role), root / 'src' / ('chrono-' + role) /
                  ('server' if role == 'grapher' else '') / ('chrono_' + role))
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return str(path.resolve())
    raise ValueError(f'missing executable chrono_{role} in {root}')
