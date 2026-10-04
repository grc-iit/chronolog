import json
from pathlib import Path
import shlex

PORT_NAMES = ('catalog', 'internal', 'raft', 'keeper', 'keeper_internal', 'grapher', 'player')
GRAPHERS = ('grapher-a', 'grapher-b')


class Lab:
    """The lab description file: hosts, addresses, role placement, archive paths, work directory, locks."""

    def __init__(self, path):
        self.path = Path(path).resolve()
        data = json.loads(self.path.read_text())
        self.data = data
        names = [host['name'] for host in data['hosts']]
        if len(names) != 3 or len(set(names)) != 3:
            raise ValueError(f'{path}: hosts must list three distinct machines, got {names}')
        if data['driver'] not in names:
            raise ValueError(f'{path}: driver {data["driver"]!r} is not a listed host')
        graphers = data['graphers']
        if len(set(graphers)) != 2 or len(graphers) != 2 or not set(graphers) <= set(names):
            raise ValueError(f'{path}: graphers must name two distinct listed hosts, got {graphers}')
        missing = [name for name in PORT_NAMES if not isinstance(data['ports'].get(name), int)]
        if missing:
            raise ValueError(f'{path}: ports lacks {missing}')
        for lock in data.get('locks', []):
            if lock['host'] not in names or not lock['command']:
                raise ValueError(f'{path}: lock {lock} needs a listed host and a command')
        self.hosts = {host['name']: host for host in data['hosts']}
        self.nodes = {name: self.hosts[name]['address'] for name in names}
        self.driver = data['driver']
        self.workdir = data['workdir']
        self.locks = data.get('locks', [])
        self.driver_env = data.get('driver_env', {})
        self.port = data['ports']
        role = dict(zip(graphers, GRAPHERS))
        self.table = [dict(node=name, ip=self.nodes[name], replica=index + 1, keeper=f'keeper-{index + 1}',
                           visor=f'visor-{index + 1}', player=f'player-{index + 1}', archive=self.archive_of(name),
                           grapher=role.get(name)) for index, name in enumerate(names)]
        p = self.port
        self.ports = {r['node']: [p['catalog'], p['internal'], p['raft'], p['keeper'], p['keeper_internal'],
                                  p['player']] + ([p['grapher']] if r['grapher'] else []) for r in self.table}
        # The Visors route story s to graphers[s % 2], so the endpoint list follows the grapher names.
        self.graphers = {name: self.row(host) for name, host in zip(GRAPHERS, graphers)}
        self.archive = self.archive_of(self.driver)

    def row(self, node):
        return next(r for r in self.table if r['node'] == node)

    def endpoint(self, row, port):
        return f"{row['ip']}:{self.port[port]}"

    def grapher_endpoints(self):
        return [self.endpoint(row, 'grapher') for row in self.graphers.values()]

    def archive_of(self, node):
        return self.hosts[node].get('archive', self.data['archive'])

    def ssh(self, node):
        return self.hosts[node].get('ssh', node)

    def archive_ready(self, node, archive=None):
        """Shell test that the shared archive's file system is present on node."""
        mount = self.hosts[node].get('mount')
        if mount:
            return f'test -d {shlex.quote(mount)} && mountpoint -q {shlex.quote(mount)}'
        return f'test -d {shlex.quote(str(Path(archive or self.archive_of(node)).parent))}'

    def hardware(self, node):
        mount = self.hosts[node].get('mount')
        return 'hostname; uname -a; lscpu' + (f'; findmnt {shlex.quote(mount)} || true' if mount else '')

    def workdir_shell(self):
        if self.workdir == '~' or self.workdir.startswith('~/'):
            rest = self.workdir[2:]
            return '"$HOME"' + ('/' + shlex.quote(rest) if rest else '')
        return shlex.quote(self.workdir)

    def configs(self, work, tag):
        p = self.port
        peers = [dict(id=r['replica'], catalog_endpoint=self.endpoint(r, 'catalog'),
                      internal_endpoint=self.endpoint(r, 'internal'), raft_endpoint=self.endpoint(r, 'raft'))
                 for r in self.table]
        keepers = [dict(process_id=r['keeper'], endpoint=self.endpoint(r, 'keeper')) for r in self.table]
        graphers = self.grapher_endpoints()
        catalog = ','.join(peer['catalog_endpoint'] for peer in peers)
        internal = ','.join(peer['internal_endpoint'] for peer in peers)
        result = {}
        for r in self.table:
            node, ip = r['node'], r['ip']
            folder = f"{work[node]}/run/{tag}"
            result[r['visor']] = (node, dict(membership_mode='dynamic', listen=f"{ip}:{p['catalog']}",
                internal_listen=f"{ip}:{p['internal']}", db_path=folder + '/catalog.sqlite', keepers=keepers,
                graphers=graphers, player=self.endpoint(self.table[0], 'player'), worker_threads=4,
                raft=dict(server_id=r['replica'], raft_endpoint=f"{ip}:{p['raft']}", peers=peers)))
            result[r['keeper']] = (node, dict(process_id=r['keeper'], listen=f"{ip}:{p['keeper']}",
                internal_listen=f"{ip}:{p['keeper_internal']}", self_endpoint=f"{ip}:{p['keeper']}",
                visor_internal=internal, wal_dir=folder + '/' + r['keeper'] + '/wal', story_chunk_duration_secs=1,
                seal_interval_ms=200, archive_visibility_delay_secs=1, watermark_resend_timeout_secs=2,
                shutdown_confirm_timeout_secs=5, worker_threads=4, heartbeat_interval_ms=200))
            result[r['player']] = (node, dict(listen=f"{ip}:{p['player']}", advertise=f"{ip}:{p['player']}",
                visor=catalog, visor_internal=internal, archive_root=r['archive'], manifest_poll_ms=200,
                keeper_internal={k['keeper']: self.endpoint(k, 'keeper_internal') for k in self.table}))
            if r['grapher']:
                result[r['grapher']] = (node, dict(process_id=r['grapher'], manifest_writer=r['grapher'],
                    internal_listen=f"{ip}:{p['grapher']}", self_endpoint=f"{ip}:{p['grapher']}",
                    visor_internal=internal, archive_root=r['archive']))
        return result

    def plan(self, tag='plan'):
        """The resolved placement without touching any host; work directories stay unexpanded."""
        return dict(config=str(self.path), driver=self.driver, workdir=self.workdir, archive=self.archive,
                    table=self.table, ports=self.ports,
                    locks=[dict(lock, ssh=self.ssh(lock['host'])) for lock in self.locks],
                    driver_env=self.driver_env, probe=self.graphers['grapher-b']['player'],
                    configs=self.configs({node: self.workdir for node in self.nodes}, tag))
