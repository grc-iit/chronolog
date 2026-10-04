"""LOCAL-1 discovery and lifetime leases for one MCP connection."""
import json
import os
from pathlib import Path
import secrets
import socket


def modules():
    try:
        from chronolog_local import cli, registry
    except ImportError as error:
        raise ValueError('local instance tools require chronolog-local 4.0.0') from error
    return cli, registry


class Instances:
    def __init__(self, args):
        self.args = args
        self.record = None
        self.folder = None
        self.lease_fd = None
        self.lease_path = None

    def listing(self, probe=False):
        cli, reg = modules()
        root = reg.home()
        records = []
        for folder in sorted((root / 'instances').iterdir()):
            if (folder / 'instance.json').exists():
                record = reg.status(folder, probe)
                holders = reg.leases(folder)
                record['attach'] = {'count': len(holders), 'holders': holders}
                records.append(record)
        records += [cli.external_status(reg.load(path)) if probe else dict(reg.load(path), state='unprobed')
                    for path in sorted((root / 'external').glob('*.json'))]
        fields = ('schema', 'id', 'name', 'kind', 'state', 'endpoints', 'owner', 'paths', 'tiers',
                  'attach', 'policy', 'clock', 'probe', 'probe_error')
        return [dict({key: record[key] for key in fields if key in record},
                     bound=self.record is not None and record['id'] == self.record['id'])
                for record in records]

    def selected(self, name, probe=False):
        cli, reg = modules()
        folder, record = reg.find(name)
        current = reg.status(folder, probe) if folder else cli.external_status(record)
        return folder, current

    def startup(self):
        if self.args.catalog:
            # chronolog run exports endpoints and the registered instance id together.
            try:
                records = self.listing()
            except ValueError:
                records = []
            selected = os.getenv('CHRONOLOG_INSTANCE')
            matches = [r for r in records if r['endpoints']['catalog'] == self.args.catalog
                       and (not selected or selected in (r['id'], r['name']))]
            if matches:
                return self.attach(matches[0]['id'])
            self.record = {'id': 'catalog:' + self.args.catalog, 'name': 'explicit', 'state': 'configured',
                           'endpoints': {'catalog': self.args.catalog, 'player': self.args.player},
                           'tiers': [], 'attach': {'count': 0, 'holders': []}, 'clock': {'status': 'Unknown'}}
            return self.record
        selected = os.getenv('CHRONOLOG_INSTANCE') or 'default'
        try:
            _, record = self.selected(selected)
            if record['state'] == 'ready':
                return self.attach(selected)
        except ValueError:
            pass
        if os.getenv('CHRONOLOG_AUTOSTART') == '1':
            self.up(selected, True, None, None)
            return self.attach(selected)
        if os.getenv('CHRONOLOG_INSTANCE'):
            return None
        try:
            with socket.create_connection(('127.0.0.1', 50051), timeout=1):
                pass
        except OSError:
            return None
        self.record = {'id': 'legacy', 'name': 'legacy', 'state': 'configured',
                       'endpoints': {'catalog': '127.0.0.1:50051', 'player': '127.0.0.1:50054'},
                       'tiers': [], 'attach': {'count': 0, 'holders': []}, 'clock': {'status': 'Unknown'}}
        return self.record

    def up(self, name, create, on_last_detach, idle_grace_s):
        cli, reg = modules()
        if not create:
            reg.find(name)
        options = ['up', name, '--label', self.args.identity or 'chronolog-mcp']
        if on_last_detach is not None:
            options += ['--on-last-detach', on_last_detach]
        if idle_grace_s is not None:
            if isinstance(idle_grace_s, bool) or idle_grace_s < 0:
                raise ValueError('idle_grace_s must be nonnegative')
            options += ['--idle-grace-s', str(idle_grace_s)]
        return cli.up(cli.parser().parse_args(options))

    def attach(self, name):
        cli, reg = modules()
        folder, record = self.selected(name)
        if record['state'] != 'ready':
            raise ValueError(f'instance {name} is {record["state"]}; use instance_control action=up')
        if folder:
            with reg.control(folder / 'run/control.lock'):
                if reg.status(folder)['state'] != 'ready':
                    raise ValueError('instance stopped before attaching')
                path = folder / 'run/attach' / (secrets.token_hex(16) + '.json')
                fd = reg.lock(path)
                try:
                    holder = {'pid': os.getpid(), 'start_time': Path('/proc/self/stat').read_text().rsplit(')', 1)[1].split()[19],
                              'label': self.args.identity or 'chronolog-mcp', 'mode': 'mcp'}
                    os.write(fd, (json.dumps(holder) + '\n').encode())
                    os.fsync(fd)
                except BaseException:
                    os.close(fd)
                    path.unlink(missing_ok=True)
                    raise
                self.lease_fd, self.lease_path = fd, path
        self.folder, self.record = folder, record
        return record

    def detach(self):
        if self.lease_fd is not None:
            self.lease_path.unlink(missing_ok=True)
            os.close(self.lease_fd)
        self.lease_fd = self.lease_path = self.folder = self.record = None

    def down(self, name, force):
        cli, _ = modules()
        options = ['down', name] + (['--force'] if force else [])
        return cli.down(cli.parser().parse_args(options))

    def status(self):
        if self.record is None:
            return None
        if self.record['name'] in ('explicit', 'legacy'):
            return self.record
        _, current = self.selected(self.record['id'])
        if self.folder:
            _, reg = modules()
            holders = reg.leases(self.folder)
            current['attach'] = {'count': len(holders), 'holders': holders}
        return {key: current.get(key) for key in ('id', 'name', 'state', 'endpoints', 'tiers', 'attach', 'clock')}
