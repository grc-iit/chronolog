#!/usr/bin/env python3
import argparse
import importlib.util
import io
import json
import os
from pathlib import Path
import random
import shlex
import signal
import socket
import subprocess
import sys
import tarfile
import time
import uuid

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
STARTUP_ATTEMPTS = 4


class StartupError(RuntimeError):
    pass


def endpoints(config):
    found = [config[key] for key in ('listen', 'internal_listen') if key in config]
    if 'raft' in config:
        found.append(config['raft']['raft_endpoint'])
    return found


def bindable(port):
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            probe.bind(('0.0.0.0', port))
        except OSError:
            return False
    return True


class Local:
    def __init__(self, args):
        self.args = args
        self.tag = 'm8e-' + uuid.uuid4().hex[:10]
        self.folder = ROOT / 'build/dynamic' / self.tag
        self.folder.mkdir(parents=True)
        self.archive = str(self.folder / 'archive')
        self.processes = {}
        self.services = {}
        self.nodes = {'dragon': '127.0.0.1'}

    def write(self, role, node, config):
        path = self.folder / (role + '.json')
        path.write_text(json.dumps(config))
        self.services[role] = (node, path, config)

    def start(self, role):
        node, path, config = self.services[role]
        kind = role.split('-')[0]
        if kind == 'proxy':
            command = ['python3', str(HERE / 'proxy.py'), config['listen'], json.dumps(config['targets']), config['blocked']]
        else:
            command = [getattr(self.args, kind), '--config', str(path)]
        log = open(self.folder / (role + '.log'), 'ab')
        env = dict(os.environ)
        if kind == 'keeper':
            env.update(grpc_proxy='http://' + self.services['proxy-' + role][2]['listen'],
                       no_grpc_proxy='', no_proxy='')
        process = subprocess.Popen(['timeout', '-k', '5', '340', *command], stdout=log, stderr=log,
                                   start_new_session=True, env=env)
        self.processes[role] = (process, log)
        self.ready(role, endpoints(config), process)

    def alive(self):
        for role, (process, _) in self.processes.items():
            if process.poll() is not None:
                raise StartupError(f'{role} exited {process.returncode} after startup: {self.tail(role)}')

    def tail(self, role):
        try:
            lines = (self.folder / (role + '.log')).read_text(errors='replace').strip().splitlines()
        except OSError:
            return 'no log'
        return ' | '.join(lines[-3:]) or 'empty log'

    def stop(self, role):
        entry = self.processes.pop(role, None)
        if entry:
            process, log = entry
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
            log.close()

    def ready(self, role, listening, process=None):
        deadline = time.monotonic() + 30
        pending = list(listening)
        while pending and time.monotonic() < deadline:
            if process is not None and process.poll() is not None:
                raise StartupError(f'{role} exited {process.returncode} during startup: {self.tail(role)}')
            host, port = pending[0].rsplit(':', 1)
            try:
                with socket.create_connection((host, int(port)), timeout=.2):
                    pending.pop(0)
            except OSError:
                time.sleep(.1)
        if pending:
            raise StartupError(f'{role} not listening on {pending[0]} after 30s: {self.tail(role)}')

    def block(self, role, value):
        path = Path(self.services['proxy-' + role][2]['blocked'])
        if value:
            path.touch()
        else:
            path.unlink(missing_ok=True)

    def close(self):
        for role in list(self.processes):
            self.stop(role)

    def ports(self):
        free = []
        for block in random.sample(range(4400), 200):
            if all(bindable(10000 + block * 5 + i) for i in range(5)):
                free.append(block)
            if len(free) == 6:
                return [[f'127.0.0.1:{10000 + block * 5 + i}' for i in range(5)] for block in free]
        raise StartupError('no free 5-port blocks in 200 random draws')


class Homelab(Local):
    def __init__(self, args):
        super().__init__(args)
        spec = importlib.util.spec_from_file_location('cluster', ROOT / 'deploy/cluster/cluster.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        self.cluster = module.Cluster()
        self.nodes = module.NODES
        self.archive = '/mnt/nfs/chronolog-sprint/' + self.tag + '/archive'
        module.PORTS = {'dragon': [50051, 50061, 50057, 50056, 50066, 50058, 50053],
                        'blade': [50051, 50061, 50057, 50053, 50054],
                        'mini': [50052, 50062, 50056, 50055, 50065, 50066]}
        self.cluster.preflight()
        self.homes = {node: self.cluster.run(node, 'printf %s "$HOME"').decode() for node in self.nodes}
        for node in ('dragon', 'blade'):
            self.cluster.run(node, 'mountpoint -q /mnt/nfs && mkdir -p ' + shlex.quote(self.archive))
        for node in self.nodes:
            (self.folder / (node + '-hardware.log')).write_bytes(
                self.cluster.run(node, 'hostname; uname -a; lscpu; findmnt /mnt/nfs || true'))

    def ports(self):
        return [[self.nodes[node] + ':' + str(port) for port in ports] for node, ports in (
            ('dragon', [50051, 50061, 50057, 50056, 50066]),
            ('dragon', [50056, 50066, 50058, 50051, 50061]),
            ('blade', [50051, 50061, 50057, 50053, 50054]),
            ('mini', [50052, 50062, 50056, 50055, 50065]),
            ('mini', [50055, 50065, 50066, 50052, 50062]),
            ('dragon', [50053, 50054, 50055, 50056, 50057]))]

    def write(self, role, node, config):
        super().write(role, node, config)
        remote = self.homes[node] + '/chronolog-sprint/run/' + self.tag
        config = json.loads(json.dumps(config).replace(str(self.folder), remote))
        self.services[role] = (node, Path(remote) / (role + '.json'), config)

    def stage(self):
        for node in self.nodes:
            buffer = io.BytesIO()
            with tarfile.open(fileobj=buffer, mode='w') as tar:
                binaries = set()
                for role, (host, path, config) in self.services.items():
                    if host != node:
                        continue
                    kind = role.split('-')[0]
                    if kind != 'proxy':
                        binaries.add(kind)
                    data = json.dumps(config).encode()
                    info = tarfile.TarInfo(f'run/{self.tag}/{role}.json')
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
                for kind in binaries:
                    tar.add(getattr(self.args, kind), arcname=f'bin/{self.tag}/chrono_{kind}')
                tar.add(HERE / 'proxy.py', arcname=f'run/{self.tag}/proxy.py')
            self.cluster.run(node, 'mkdir -p ~/chronolog-sprint && cd ~/chronolog-sprint && tar xf -',
                             60, buffer.getvalue())

    def start(self, role):
        node, path, config = self.services[role]
        kind = role.split('-')[0]
        if kind == 'proxy':
            command = ['python3', str(path.parent / 'proxy.py'), config['listen'], json.dumps(config['targets']), config['blocked']]
        else:
            command = [self.homes[node] + f'/chronolog-sprint/bin/{self.tag}/chrono_{kind}', '--config', str(path)]
        if kind == 'keeper':
            command = ['env', 'grpc_proxy=http://' + self.services['proxy-' + role][2]['listen'],
                       'no_grpc_proxy=', 'no_proxy=', *command]
        self.cluster.launch(role, node, 'exec ' + shlex.join(command) + ' >>' + shlex.quote(str(path.with_suffix('.log'))) + ' 2>&1', 340)
        self.ready(role, endpoints(config))

    def stop(self, role):
        self.cluster.stop(role)

    def alive(self):
        pass

    def tail(self, role):
        return 'see the node log'

    def block(self, role, value):
        node, _, config = self.services['proxy-' + role]
        self.cluster.run(node, ('touch ' if value else 'rm -f ') + shlex.quote(config['blocked']))

    def close(self):
        for role, (node, path, _) in self.services.items():
            try:
                (self.folder / (role + '.log')).write_bytes(self.cluster.run(node, 'cat ' + shlex.quote(str(path.with_suffix('.log'))), 10))
            except Exception:
                pass
        self.cluster.close()
        for node in self.nodes:
            paths = [f'~/chronolog-sprint/bin/{self.tag}', f'~/chronolog-sprint/run/{self.tag}']
            if node in ('dragon', 'blade'):
                paths.append('/mnt/nfs/chronolog-sprint/' + self.tag)
            try:
                self.cluster.run(node, 'rm -rf ' + ' '.join(paths), 30)
            except Exception:
                pass


def configure(stack):
    ports = stack.ports()
    peers = [dict(id=i + 1, catalog_endpoint=p[0], internal_endpoint=p[1], raft_endpoint=p[2])
             for i, p in enumerate(ports[:3])]
    keepers = [dict(process_id='keeper-' + str(i + 1), endpoint=p[0]) for i, p in enumerate(ports[3:5])]
    internal = ','.join(peers[i]['internal_endpoint'] for i in (2, 0, 1))
    catalog = ','.join(peers[i]['catalog_endpoint'] for i in (2, 0, 1))
    graphers = [ports[5][0]]
    player = ports[5][1]
    if isinstance(stack, Homelab):
        graphers += [stack.nodes['blade'] + ':50053']
        player = stack.nodes['blade'] + ':50054'
    for i, peer in enumerate(peers):
        role = 'visor-' + str(i + 1)
        node = 'blade' if isinstance(stack, Homelab) and i == 2 else 'dragon'
        stack.write(role, node, dict(membership_mode='dynamic', listen=peer['catalog_endpoint'],
                    internal_listen=peer['internal_endpoint'], db_path=str(stack.folder / (role + '.sqlite')),
                    keepers=keepers, graphers=graphers, player=player, keeper_failure_timeout_ms=1500,
                    release_fence_timeout_ms=1000, worker_threads=4,
                    raft=dict(server_id=i + 1, raft_endpoint=peer['raft_endpoint'], peers=peers,
                              election_lower_ms=300 if i == 2 else 1200, election_upper_ms=400 if i == 2 else 1600)))
    for i, p in enumerate(ports[3:5]):
        role = 'keeper-' + str(i + 1)
        node = 'mini' if isinstance(stack, Homelab) else 'dragon'
        stack.write('proxy-' + role, node, dict(listen=p[2], targets=[peer['internal_endpoint'] for peer in peers],
                                              blocked=str(stack.folder / (role + '.blocked'))))
        stack.write(role, node, dict(process_id=role, listen=p[0], internal_listen=p[1], self_endpoint=p[0],
                    visor_internal=internal, wal_dir=str(stack.folder / role / 'wal'), story_chunk_duration_secs=1,
                    seal_interval_ms=100, archive_visibility_delay_secs=1, watermark_resend_timeout_secs=1,
                    shutdown_confirm_timeout_secs=1, worker_threads=4, heartbeat_interval_ms=100,
                    append_ceiling_wait_ms=100, retention_cap_mb=32, wal_max_bytes=67108864,
                    wal_segment_bytes=8388608))
    for i, endpoint in enumerate(graphers):
        role = 'grapher-' + ('a' if i == 0 else 'b')
        stack.write(role, 'dragon' if i == 0 else 'blade', dict(process_id=role, manifest_writer=role,
                    internal_listen=endpoint, self_endpoint=endpoint, visor_internal=internal,
                    archive_root=stack.archive, heartbeat_interval_ms=200))
    stack.write('player', 'blade' if isinstance(stack, Homelab) else 'dragon',
                dict(listen=player, advertise=player, visor=catalog,
                     visor_internal=internal, archive_root=stack.archive,
                     keeper_internal={k['process_id']: p[1] for k, p in zip(keepers, ports[3:5])},
                     keeper_deadline_ms=300, manifest_poll_ms=100))
    return peers


def main():
    parser = argparse.ArgumentParser()
    for role in ('rpc', 'visor', 'keeper', 'grapher', 'player'):
        parser.add_argument('--' + role, required=True)
    parser.add_argument('--homelab', action='store_true')
    args = parser.parse_args()
    def interrupted(signum, frame):
        raise RuntimeError('driver interrupted by signal ' + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    stack = None
    try:
        for attempt in range(1, STARTUP_ATTEMPTS + 1):
            try:
                stack = Homelab(args) if args.homelab else Local(args)
                peers = configure(stack)
                if args.homelab:
                    stack.stage()
                for role in stack.services:
                    stack.start(role)
                stack.alive()
                break
            except StartupError as error:
                print(f'STARTUP attempt {attempt}/{STARTUP_ATTEMPTS} failed: {error}', flush=True)
                if stack:
                    stack.close()
                    print('Logs ' + str(stack.folder), flush=True)
                    stack = None
                if args.homelab or attempt == STARTUP_ATTEMPTS:
                    print('FAIL startup: ' + str(error), flush=True)
                    return 1
        from scenario import Scenario
        scenario = Scenario(stack, peers, args.rpc)
        try:
            scenario.run()
        except Exception as error:
            line = f'FAIL scenario {scenario.phase} {time.monotonic() - scenario.phase_started:.3f}s {error}'
            with open(stack.folder / 'scenarios.log', 'a') as log:
                log.write(line + '\n')
            print(line, flush=True)
            return 1
        finally:
            scenario.close()
        return 0
    except Exception as error:
        print('FAIL driver: ' + str(error), flush=True)
        return 1
    finally:
        if stack:
            stack.close()
            print('Logs ' + str(stack.folder), flush=True)


if __name__ == '__main__':
    sys.exit(main())
