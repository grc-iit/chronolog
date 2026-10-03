import ctypes
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import socket
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
                host, port = endpoints[key].rsplit(':', 1)
                with socket.create_connection((host, int(port)), timeout=1):
                    pass
        return 'tcp'
    client = chronolog.connect(endpoints['catalog'], player=endpoints.get('player'), timeout=1, max_retries=0)
    try:
        client.list_chronicles(timeout=1)
        try:
            list(client.read(2**64 - 1, timeout=1))
        except Exception as error:
            if not any(word in str(error).upper() for word in ('NOT_FOUND', 'FAILED_PRECONDITION')):
                raise
    finally:
        del client
    return 'rpc'


def status(folder, probing=False):
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
    raise ValueError(f'no instance {name}; start one with: chronolog up {name} --bin-dir <directory>')


def free_ports(endpoints):
    sockets = []
    try:
        for address in endpoints.values():
            host, port = address.rsplit(':', 1)
            sock = socket.socket()
            sockets.append(sock)
            try:
                sock.bind((host, int(port)))
            except OSError as error:
                raise ValueError(f'cannot bind fixed endpoint {address}: {error}; inspect holder with ss -ltnp') from error
    finally:
        for sock in sockets:
            sock.close()


def binary(bin_dir, role):
    root = Path(bin_dir)
    candidates = (root / ('chrono_' + role), root / 'src' / ('chrono-' + role) /
                  ('server' if role in ('keeper', 'grapher') else '') / ('chrono_' + role))
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return str(path.resolve())
    raise ValueError(f'missing executable chrono_{role} in {root}')
