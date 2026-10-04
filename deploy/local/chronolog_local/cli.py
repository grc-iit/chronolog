import argparse
import json
import os
from pathlib import Path
import secrets
import shlex
import shutil
import signal
import socket
import sys
import time
import getpass

from .registry import (KEYS, ROLES, atomic, binary, boot_id, control, directory, find, free_ports, home,
                       leases, load, lock, name_check, probe, process_gone, status)
from .supervisor import configs, detach
from .tiers import add_marker, initialize_local


def create(args):
    root = home()
    name_check(args.name)
    with control(root / 'locks/registry.lock'):
        folder = root / 'instances' / args.name
        if folder.exists() or (root / 'external' / (args.name + '.json')).exists():
            raise ValueError('instance already exists: ' + args.name)
        bin_dir = args.bin_dir or os.environ.get('CHRONOLOG_BIN_DIR')
        for role in ROLES:
            binary(bin_dir, role)
        endpoints = None
        for attempt in range(3):
            port = args.port_base if args.port_base else 61000 + secrets.randbelow((65535 - 61000 - 7) // 8) * 8
            if port < 1024 or port + 5 > 65535:
                raise ValueError('port block outside 1024..65535')
            candidate = {key: f'{args.bind}:{port + offset}' for offset, key in enumerate(KEYS)}
            try:
                free_ports(candidate)
                endpoints = candidate
                break
            except ValueError:
                if args.port_base or attempt == 2:
                    raise
        directory(folder)
        for child in ('config', 'visor', 'keeper/wal', 'grapher/archive', 'player/cache', 'logs', 'run/attach'):
            directory(folder / child)
        wal = directory(args.wal_dir or folder / 'keeper/wal').resolve()
        archive = directory(args.local_root or folder / 'grapher/archive').resolve()
        record = {'schema': 'chronolog.instance/v1', 'id': secrets.token_hex(16), 'name': args.name,
                  'kind': 'managed', 'version': '4.0.0', 'created_ns': time.time_ns(),
                  'owner': {'uid': os.getuid(), 'user': getpass.getuser(), 'created_by': args.label},
                  'endpoints': endpoints, 'paths': {'catalog_db': str(folder / 'visor/catalog.sqlite'),
                  'wal_dir': str(wal), 'logs': str(folder / 'logs')},
                  'tiers': [{'name': 'local', 'kind': 'posix', 'root': str(archive), 'rank': 0, 'budget_bytes': args.budget_bytes}],
                  'tier_io_timeout_ms': args.tier_io_timeout_ms,
                  'tier_probe_interval_ms': args.tier_probe_interval_ms,
                  'policy': {'on_last_detach': 'stop' if args.ephemeral else args.on_last_detach,
                             'idle_grace_s': args.idle_grace_s if args.idle_grace_s is not None else (30 if args.ephemeral else 300)},
                  'bin_dir': str(Path(bin_dir).resolve()) if bin_dir else None, 'insecure_bind_all': args.insecure_bind_all,
                  'overrides': json.loads(args.overrides)}
        try:
            if args.bind != '127.0.0.1' and not args.insecure_bind_all:
                raise ValueError('non-loopback bind requires --insecure-bind-all')
            if record['policy']['idle_grace_s'] < 0 or args.budget_bytes < 0:
                raise ValueError('grace and budget must be nonnegative')
            if args.tier_io_timeout_ms <= 0 or args.tier_probe_interval_ms <= 0:
                raise ValueError('tier probe deadlines and intervals must be positive')
            configs(record)
            initialize_local(record)
            atomic(folder / 'instance.json', record)
        except Exception:
            shutil.rmtree(folder)
            raise
        return folder, record


def tier_add(args):
    name_check(args.tier_name)
    if args.tier_name == 'local' or args.rank <= 0:
        raise ValueError('slow tiers require a name other than local and a positive rank')
    folder, _ = find(args.name)
    if folder is None:
        raise ValueError('external instances have no managed tiers')
    with control(folder / 'run/control.lock'):
        record = load(folder / 'instance.json')
        if any(tier['name'] == args.tier_name or tier.get('rank', 0) == args.rank
               for tier in record['tiers']):
            raise ValueError('tier name and rank must be unique')
        root = Path(args.root).expanduser().absolute()
        if folder.resolve().is_relative_to(root.resolve()):
            # The Grapher refuses a tier_status_file under a slow tier root (I13.15); the instance folder holds it.
            raise ValueError('tier root must not contain the instance folder')
        root.mkdir(mode=0o700, parents=True, exist_ok=True)
        if any(os.path.samefile(root, tier['root']) for tier in record['tiers']):
            raise ValueError('tier root is already configured')
        deployment_id = record.get('deployment_id', record['id'])
        tier = add_marker({'name': args.tier_name, 'root': str(root), 'rank': args.rank,
                           'kind': 'posix'}, deployment_id)
        if 'deployment_id' not in record:
            initialize_local(record)
        record['tiers'] = sorted(record['tiers'] + [tier], key=lambda item: item['rank'])
        atomic(folder / 'instance.json', record)
        return tier


def tier_ls(args):
    folder, _ = find(args.name)
    if folder is None:
        raise ValueError('external instances have no managed tiers')
    return status(folder)['tiers']


def remap_unbooted(folder, record, args):
    if record.get('booted') or args.port_base or getattr(args, '_bind_attempt', 0) >= 2:
        return False
    args._bind_attempt = getattr(args, '_bind_attempt', 0) + 1
    port = 61000 + secrets.randbelow((65535 - 61000 - 7) // 8) * 8
    host = record['endpoints']['catalog'].rsplit(':', 1)[0]
    record['endpoints'] = {key: f'{host}:{port + offset}' for offset, key in enumerate(KEYS)}
    atomic(folder / 'instance.json', record)
    return True


def up(args):
    if not hasattr(args, '_boot_deadline'):
        args._boot_deadline = time.monotonic() + 30
    try:
        folder, record = find(args.name)
    except ValueError:
        folder, record = create(args)
    if folder is None:
        probe(record)
        return dict(record, state='ready')
    with control(folder / 'run/control.lock'):
        current = status(folder)
        if current['state'] == 'stopped':
            try:
                previous = load(folder / 'run/status.json')
            except FileNotFoundError:
                previous = {}
            if previous.get('boot_id') == boot_id():
                pids = [service['pid'] for service in previous.get('services', {}).values() if service.get('pid')]
                while not all(process_gone(pid) for pid in pids):
                    if time.monotonic() >= args._boot_deadline:
                        raise ValueError('30 s readiness deadline waiting for previous services to exit')
                    time.sleep(0.05)
            # Preserve endpoints and identities; an explicit binary directory can select an upgrade.
            if args.bin_dir:
                record['bin_dir'] = str(Path(args.bin_dir).resolve())
                atomic(folder / 'instance.json', record)
            while True:
                try:
                    free_ports(record['endpoints'])
                    break
                except ValueError:
                    if not remap_unbooted(folder, record, args):
                        raise
            (folder / 'run/status.json').unlink(missing_ok=True)
            detach(folder)
    deadline = args._boot_deadline
    while time.monotonic() < deadline:
        current = status(folder)
        if current['state'] == 'ready':
            return current
        if current.get('error'):
            break
        if current['state'] == 'stopped' and (folder / 'run/status.json').exists():
            if load(folder / 'run/status.json').get('error'):
                break
        time.sleep(0.05)
    logs = []
    for role in ROLES:
        path = folder / 'logs' / (role + '.log')
        if path.exists():
            logs.append(role + ':\n' + '\n'.join(path.read_text(errors='replace').splitlines()[-40:]))
    # status() deliberately ignores stale status; retain startup errors for this invocation only.
    last = load(folder / 'run/status.json') if (folder / 'run/status.json').exists() else {}
    if 'cannot listen on' in '\n'.join(logs) and not record.get('booted'):
        while time.monotonic() < deadline and status(folder)['state'] != 'stopped':
            time.sleep(0.05)
        if time.monotonic() < deadline:
            with control(folder / 'run/control.lock'):
                record = load(folder / 'instance.json')
                if status(folder)['state'] == 'stopped' and remap_unbooted(folder, record, args):
                    retry = True
                else:
                    retry = False
            if retry:
                return up(args)
    raise ValueError(last.get('error', '30 s readiness deadline') + '\n' + '\n'.join(logs))


def down(args):
    folder, record = find(args.name)
    if folder is None:
        raise ValueError('external instances cannot be stopped by this launcher')
    with control(folder / 'run/control.lock'):
        current = status(folder)
        if current['state'] != 'stopped':
            holders = [item for item in leases(folder) if item['pid'] != os.getpid()]
            if holders and not args.force:
                raise ValueError('foreign attach leases: ' + json.dumps(holders))
            deadline = time.monotonic() + 30
            while not current.get('supervisor_pid') and current['state'] != 'stopped':
                if time.monotonic() >= deadline:
                    raise ValueError('supervisor has not published its starting status')
                time.sleep(0.05)
                current = status(folder)
            if current['state'] != 'stopped':
                os.kill(current['supervisor_pid'], signal.SIGTERM)
    keeper = configs(record)['keeper'].get('shutdown_confirm_timeout_secs', 150)
    grapher = configs(record)['grapher'].get('drain_timeout_ms', 5000) / 1000
    deadline = time.monotonic() + keeper + grapher + 40
    while time.monotonic() < deadline:
        current = status(folder)
        if current['state'] == 'stopped':
            if args.purge:
                with control(home() / 'locks/registry.lock'):
                    with control(folder / 'run/control.lock'):
                        if status(folder)['state'] != 'stopped':
                            raise ValueError('instance restarted before purge')
                        shutil.rmtree(folder)
            return current
        time.sleep(0.05)
    raise ValueError('ordered shutdown deadline; inspect supervisor log')


def resolve(args):
    if os.environ.get('CHRONOLOG_CATALOG'):
        return None, {'id': 'environment', 'name': 'environment', 'endpoints': {
            'catalog': os.environ['CHRONOLOG_CATALOG'], 'player': os.environ.get('CHRONOLOG_PLAYER', '')}}
    args.name = args.name or os.environ.get('CHRONOLOG_INSTANCE') or 'default'
    try:
        folder, record = find(args.name)
        if folder is None or status(folder)['state'] == 'ready':
            return folder, record
    except ValueError:
        pass
    if args.up or os.environ.get('CHRONOLOG_AUTOSTART') == '1':
        up(args)
        return find(args.name)
    # A named or id-selected instance must not silently fall through to another deployment.
    if args.name != 'default' or os.environ.get('CHRONOLOG_INSTANCE'):
        raise ValueError(f'instance {args.name} is not ready; run: chronolog up {args.name}')
    try:
        with socket.create_connection(('127.0.0.1', 50051), timeout=1):
            pass
        return None, {'id': 'legacy', 'name': 'legacy', 'endpoints': {
            'catalog': '127.0.0.1:50051', 'player': '127.0.0.1:50054'}}
    except OSError:
        raise ValueError('no instance; run: chronolog up default --bin-dir <directory>') from None


def environment(record):
    return {'CHRONOLOG_HOME': str(home()), 'CHRONOLOG_CATALOG': record['endpoints']['catalog'],
            'CHRONOLOG_PLAYER': record['endpoints'].get('player', ''),
            'CHRONOLOG_INSTANCE': record['id'], 'CHRONOLOG_MCP_LOCK_DIR': str(home() / 'locks')}


def run(args):
    folder, record = resolve(args)
    command = args.command
    if command and command[0] == '--':
        command = command[1:]
    if not command:
        raise ValueError('run requires -- <command>')
    if folder:
        with control(folder / 'run/control.lock'):
            if status(folder)['state'] != 'ready':
                raise ValueError('instance stopped before attaching')
            path = folder / 'run/attach' / (secrets.token_hex(16) + '.json')
            fd = lock(path)
            atomic_record = {'pid': os.getpid(), 'start_time': Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19],
                             'label': args.label, 'mode': 'run'}
            # Do not replace an inode whose flock is the lease authority.
            os.write(fd, (json.dumps(atomic_record) + '\n').encode())
            os.fsync(fd)
            os.set_inheritable(fd, True)
    os.environ.update(environment(record))
    os.execvpe(command[0], command, os.environ)


def parser():
    result = argparse.ArgumentParser(prog='chronolog')
    sub = result.add_subparsers(dest='action', required=True)
    for action in ('create', 'up', 'run', 'env'):
        p = sub.add_parser(action)
        p.add_argument('name', nargs='?', default=None if action in ('run', 'env') else 'default')
        p.add_argument('--bin-dir')
        p.add_argument('--bind', default='127.0.0.1')
        p.add_argument('--insecure-bind-all', action='store_true')
        p.add_argument('--port-base', type=int)
        p.add_argument('--wal-dir')
        p.add_argument('--local-root')
        p.add_argument('--budget-bytes', type=int, default=0)
        p.add_argument('--tier-io-timeout-ms', type=int, default=1000)
        p.add_argument('--tier-probe-interval-ms', type=int, default=5000)
        p.add_argument('--ephemeral', action='store_true')
        p.add_argument('--on-last-detach', choices=('keep', 'stop'), default='keep')
        p.add_argument('--idle-grace-s', type=float)
        p.add_argument('--overrides', default='{}')
        p.add_argument('--label', default='shell')
        if action in ('run', 'env'):
            p.add_argument('--up', action='store_true')
    tier = sub.add_parser('tier').add_subparsers(dest='tier_action', required=True)
    p = tier.add_parser('add')
    p.add_argument('name')
    p.add_argument('tier_name')
    p.add_argument('root')
    p.add_argument('--rank', type=int, required=True)
    p.add_argument('--kind', choices=('slow',), required=True)
    p = tier.add_parser('ls')
    p.add_argument('name', nargs='?', default='default')
    p = sub.add_parser('doctor')
    p.add_argument('--bin-dir')
    p = sub.add_parser('down')
    p.add_argument('name', nargs='?', default='default')
    p.add_argument('--force', action='store_true')
    p.add_argument('--purge', action='store_true')
    for action in ('status', 'logs'):
        p = sub.add_parser(action)
        p.add_argument('name', nargs='?', default='default')
        if action == 'status':
            p.add_argument('--probe', action='store_true')
        else:
            p.add_argument('--service', choices=('supervisor',) + ROLES, default='supervisor')
            p.add_argument('--lines', type=int, default=40)
    p = sub.add_parser('ls')
    p.add_argument('--probe', action='store_true')
    p = sub.add_parser('register')
    p.add_argument('name')
    p.add_argument('--catalog', required=True)
    p.add_argument('--player')
    return result


def main():
    os.umask(0o077)
    # REMAINDER preserves the child's arguments; split at -- so options after a name work too.
    argv = sys.argv[1:]
    command = None
    if argv and argv[0] == 'run' and '--' in argv:
        index = argv.index('--')
        command, argv = argv[index + 1:], argv[:index]
    args = parser().parse_args(argv)
    if args.action == 'run':
        args.command = command or []
    try:
        if args.action == 'doctor':
            output = {'binaries': {role: binary(args.bin_dir, role) for role in ROLES}}
        elif args.action == 'tier':
            output = tier_add(args) if args.tier_action == 'add' else tier_ls(args)
        elif args.action == 'create':
            output = create(args)[1]
        elif args.action == 'up':
            output = up(args)
        elif args.action == 'down':
            output = down(args)
        elif args.action == 'run':
            run(args)
            return
        elif args.action == 'env':
            _, record = resolve(args)
            for key, value in environment(record).items():
                print('export ' + key + '=' + shlex.quote(value))
            return
        elif args.action == 'status':
            folder, record = find(args.name)
            output = status(folder, args.probe) if folder else external_status(record)
        elif args.action == 'ls':
            root = home()
            output = [status(folder, args.probe) for folder in sorted((root / 'instances').iterdir())
                      if (folder / 'instance.json').exists()]
            output += [external_status(load(path)) for path in sorted((root / 'external').glob('*.json'))]
        elif args.action == 'logs':
            folder, _ = find(args.name)
            if folder is None:
                raise ValueError('external instance has no managed logs')
            print('\n'.join((folder / 'logs' / (args.service + '.log')).read_text().splitlines()[-args.lines:]))
            return
        else:
            root = home()
            name_check(args.name)
            with control(root / 'locks/registry.lock'):
                path = root / 'external' / (args.name + '.json')
                if path.exists() or (root / 'instances' / args.name).exists():
                    raise ValueError('instance already exists')
                output = {'schema': 'chronolog.instance/v1', 'id': secrets.token_hex(16),
                          'name': args.name, 'kind': 'external', 'endpoints': {'catalog': args.catalog}}
                if args.player:
                    output['endpoints']['player'] = args.player
                atomic(path, output)
        print(json.dumps(output))
    except (ValueError, OSError, RuntimeError) as error:
        print('chronolog: ' + str(error), file=sys.stderr)
        sys.exit(1)


def external_status(record):
    for key in ('catalog', 'player'):
        if key not in record['endpoints']:
            continue
        last = None
        for endpoint in record['endpoints'][key].split(','):
            host, port = endpoint.removeprefix('dns:///').rsplit(':', 1)
            try:
                with socket.create_connection((host, int(port)), timeout=1):
                    pass
                last = None
                break
            except OSError as error:
                last = error
        if last is not None:
            return dict(record, state='stopped', probe_error=str(last))
    try:
        method = probe(record)
        return dict(record, state='ready', probe=method)
    except Exception as error:
        return dict(record, state='degraded', probe_error=str(error))
