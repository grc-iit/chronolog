#!/usr/bin/env python3
"""Multi-node lab harness: stages native services on three hosts over SSH and runs scenario.py.

Usage:
  cluster.py --config LAB.json --plan            print the resolved placement and service configs, touch nothing
  cluster.py --config LAB.json --preflight-only  take the locks, check archive mounts and service-port reachability
  cluster.py --config LAB.json                   full run; logs and results under build/cluster/<tag>

LAB.json (lab.example.json is a generic three-node lab, homelab.json the sprint homelab) names every host
(name, address, optional ssh target, optional NFS mount and archive path), the driver host whose commands run
locally, the two Grapher hosts (grapher-a, grapher-b), the remote work directory, the service ports and the
optional per-host lock commands. Each host runs a Visor replica, a Keeper and a Player; the first host's Player
is the Visors' default. A lock command runs on its host, must block until the lock is held, and keeps it until
the driver exits. driver_env lists environment variables the driver requires, for a lock the caller holds.
Prerequisites: build/dev binaries, the Python SDK python_package target, passwordless ssh to every other host,
systemd user scopes on every host, and the shared archive mounted on every host.
"""
import argparse
import io
import json
import os
from pathlib import Path
import shlex
import signal
import select
import socket
import subprocess
import sys
import tarfile
import threading
import time
import uuid

from topology import Lab
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/cluster'


class Cluster:
    def __init__(self, lab):
        self.lab = lab
        self.work = {}
        self.tag = 'cl-a7-' + uuid.uuid4().hex[:10]
        global OUT
        OUT = OUT / self.tag
        os.environ['CHRONOLOG_CLUSTER_OUT'] = str(OUT)
        self.processes = {}
        self.services = {}
        self.lock = threading.RLock()
        self.archive_owned = False
        self.build_locks = []
        OUT.mkdir(parents=True, exist_ok=True)
        self.socket_path = str(OUT / 'control.sock')

    def command(self, node, command, seconds=30, unit=None):
        scoped = ['systemd-run', '--user', '--scope', '--quiet', '-p', 'MemoryMax=4G',
                  '-p', 'MemorySwapMax=0']
        if unit:
            scoped += ['--unit', unit]
        scoped += ['timeout', '-k', '5', str(seconds), 'bash', '-c', command]
        if node == self.lab.driver:
            return scoped
        return ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', self.lab.ssh(node), shlex.join(scoped)]

    def run(self, node, command, seconds=30, data=None):
        result = subprocess.run(self.command(node, command, seconds), input=data, capture_output=True,
                                timeout=seconds + 15)
        if result.returncode:
            raise RuntimeError(f'{node} command exited {result.returncode}: {result.stderr.decode(errors="replace")}')
        return result.stdout

    def launch(self, name, node, command, seconds=900):
        with self.lock:
            unit = f'{self.tag}-{name}.scope'
            log = open(OUT / f'{name}.transport.log', 'ab')
            process = subprocess.Popen(self.command(node, command, seconds, unit), stdout=log, stderr=log)
            self.processes[name] = (node, unit, process, log)

    def stop(self, name):
        with self.lock:
            entry = self.processes.pop(name, None)
            if not entry:
                return
            node, unit, process, log = entry
            try:
                self.run(node, shlex.join(['systemctl', '--user', 'kill', '--signal=SIGKILL', unit]), 10)
            except Exception:
                pass
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            log.close()

    def ready(self, endpoint, seconds=30):
        host, port = endpoint.split(':')
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            try:
                with socket.create_connection((host, int(port)), timeout=1):
                    return
            except OSError:
                time.sleep(.2)
        raise RuntimeError(f'health timeout {endpoint}')

    def resolve(self):
        if not self.work:
            self.work = {node: self.run(node, 'printf %s ' + self.lab.workdir_shell()).decode()
                         for node in self.lab.nodes}
        return self.work

    def preflight(self, ports=None):
        ports = ports or self.lab.ports
        nodes = self.lab.nodes
        for row in self.lab.table:
            node = row['node']
            try:
                self.run(node, self.lab.archive_ready(node), 10)
            except Exception as error:
                raise RuntimeError(f'shared archive parent unavailable on {node}') from error
        server = 'import socket,time; ports=PORTS; sockets=[]\nfor p in ports:\n s=socket.socket(); s.bind(("IP",p)); s.listen(); sockets.append(s)\ntime.sleep(90)'
        try:
            for node, listed in ports.items():
                code = server.replace('PORTS', repr(listed)).replace('IP', nodes[node])
                self.launch('probe-' + node, node, 'exec python3 -c ' + shlex.quote(code), 90)
            time.sleep(2)
            for source in nodes:
                for target, listed in ports.items():
                    if source == target:
                        continue
                    for port in listed:
                        code = f'import socket; socket.create_connection(({nodes[target]!r},{port}),timeout=3).close()'
                        try:
                            self.run(source, 'exec python3 -c ' + shlex.quote(code), 6)
                        except Exception as error:
                            raise RuntimeError(f'reachability {source}->{target}:{port}: {error}') from error
            print('PASS reachability every pair on service ports', flush=True)
        finally:
            for node in nodes:
                self.stop('probe-' + node)

    def acquire_locks(self):
        for name, value in self.lab.driver_env.items():
            if os.environ.get(name) != value:
                raise RuntimeError(f'the driver on {self.lab.driver} requires {name}={value}')
        for lock in self.lab.locks:
            node = lock['host']
            unit = f'{self.tag}-build-lock-{node}.scope'
            process = subprocess.Popen(self.command(node,
                lock['command'] + '; echo LOCKED; cat >/dev/null', 900, unit),
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            self.build_locks.append((node, unit, process))
            for _ in range(26):
                if select.select([process.stdout], [], [], 30)[0]:
                    if process.stdout.readline().strip() != 'LOCKED':
                        raise RuntimeError(f'build lock holder exited on {node}')
                    break
                print(f'Waiting for {node} build lock', flush=True)
            else:
                raise RuntimeError(f'build lock unavailable on {node} within the run bound')
        if self.lab.driver_env or self.lab.locks:
            held = [self.lab.driver] if self.lab.driver_env else []
            print('PASS build locks held on ' + ' '.join(held + [lock['host'] for lock in self.lab.locks]), flush=True)

    def stage(self):
        lab, work = self.lab, self.resolve()
        probe = lab.graphers['grapher-b']['node']
        for role, (node, config) in lab.configs(work, self.tag).items():
            self.services[role] = (node, f'{work[node]}/run/{self.tag}', json.dumps(config))
        for node in lab.nodes:
            name = 'stage-' + node
            unit = f'{self.tag}-{name}.scope'
            log = open(OUT / f'{name}.transport.log', 'ab')
            home = shlex.quote(work[node])
            receiver = subprocess.Popen(self.command(node,
                f'mkdir -p {home}/bin && cd {home} && tar xf -', 60, unit),
                stdin=subprocess.PIPE, stdout=log, stderr=log)
            self.processes[name] = (node, unit, receiver, log)
            try:
                with tarfile.open(fileobj=receiver.stdin, mode='w|') as tar:
                    if node == probe:
                        tar.add(ROOT / 'build/dev/tools/lab-cluster/cluster_manifest_probe', arcname='bin/cluster_manifest_probe')
                    tar.add(ROOT / 'tools/lab-cluster/agent.py', arcname=f'run/{self.tag}/agent.py')
                    tar.add(ROOT / 'build/python/client/python/binding/package/chronolog',
                            arcname=f'run/{self.tag}/sdk/chronolog')
                    tar.add(OUT / 'stubs', arcname=f'run/{self.tag}/stubs')
                    tar.add(ROOT / 'tests/smoke/python/requirements.txt', arcname=f'run/{self.tag}/requirements.txt')
                    added = set()
                    for role, (host, folder, config) in self.services.items():
                        if host != node:
                            continue
                        binary = 'chrono_' + role.split('-')[0]
                        candidates = list((ROOT / 'build/dev/src').rglob(binary))
                        candidates = [p for p in candidates if p.is_file() and os.access(p, os.X_OK)]
                        if len(candidates) != 1:
                            raise RuntimeError(f'cannot locate unique binary {binary}: {candidates}')
                        if binary not in added:
                            tar.add(candidates[0], arcname='bin/' + binary)
                            added.add(binary)
                        info = tarfile.TarInfo(f'run/{self.tag}/{role}.json')
                        encoded = config.encode()
                        info.size = len(encoded)
                        tar.addfile(info, io.BytesIO(encoded))
                receiver.stdin.close()
                if receiver.wait(timeout=75):
                    raise RuntimeError(f'staging failed on {node}: see {OUT / (name + ".transport.log")}')
            finally:
                if not receiver.stdin.closed:
                    try:
                        receiver.stdin.close()
                    except BrokenPipeError:
                        pass
                self.stop(name)
        for node in lab.nodes:
            self.run(node, f'cd {self.folder(node)} && python3 -m venv {self.venv(node)} && '
                     f'{self.venv(node)}/bin/pip install -r requirements.txt >pip.log 2>&1', 120)
            (OUT / f'{node}-hardware.log').write_bytes(self.run(node, lab.hardware(node)))
        for node in lab.nodes:
            self.launch('sdk-agent-' + node, node, f'cd {self.folder(node)} && '
                f'exec env PYTHONPATH=sdk {self.venv(node)}/bin/python '
                'agent.py --serve >>sdk-agent.log 2>&1', 820)
            self.run(node, f'cd {self.folder(node)} && '
                'for attempt in $(seq 1 300); do test ! -S agent.sock || exit 0; sleep .1; done; '
                'cat sdk-agent.log >&2; exit 1', 35)
        for row in lab.graphers.values():
            archive = shlex.quote(row['archive'])
            self.run(row['node'], f'{lab.archive_ready(row["node"])} && mkdir -p {archive} && '
                     f'test -z "$(ls -A {archive})"')
        self.archive_owned = True
        marker = shlex.quote(f'{lab.archive}/{self.tag}.probe')
        self.run(lab.driver, f'printf %s {self.tag} >{marker}', 10)
        for row in lab.table:
            self.run(row['node'], f'test "$(cat {shlex.quote(row["archive"] + "/" + self.tag + ".probe")})" = {self.tag}', 10)
        self.run(lab.driver, f'rm {marker}', 10)
        (OUT / 'deployment.json').write_text(json.dumps({'tag': self.tag, 'services': self.services, 'topology': lab.table}, indent=2))

    def folder(self, node):
        return shlex.quote(f'{self.work[node]}/run/{self.tag}')

    def venv(self, node):
        return shlex.quote(f'{self.work[node]}/build/cluster/{self.tag}/venv')

    def start(self, role):
        node, folder, _ = self.services[role]
        binary = 'chrono_' + role.split('-')[0]
        command = (f'cd {shlex.quote(folder)} && exec {shlex.quote(self.work[node] + "/bin/" + binary)} '
                   f'--config {role}.json >>{role}.log 2>&1')
        self.launch(role, node, command)
        cfg = json.loads(self.services[role][2])
        self.ready(cfg.get('listen', cfg.get('internal_listen')))

    def control(self, request):
        roles = {'chrono-keeper': [r['keeper'] for r in self.lab.table], 'chrono-grapher': list(self.lab.graphers)}
        op, service = request
        if op == 'agent':
            node = service['node']
            data = json.dumps(service).encode()
            return self.run(node, f'cd {self.folder(node)} && '
                            f'exec env PYTHONPATH=sdk {self.venv(node)}/bin/python agent.py', 90, data).decode()
        if op in ('pause', 'resume'):
            node, unit, _, _ = self.processes[service]
            sig = 'SIGSTOP' if op == 'pause' else 'SIGCONT'
            self.run(node, shlex.join(['systemctl', '--user', 'kill', '--signal=' + sig, unit]), 10)
            return ''
        if op == 'transfer-log':
            self.collect()
            return ''.join((OUT / (keeper + '.log')).read_text() for keeper in (r['keeper'] for r in self.lab.table))
        # The probe shares grapher-b's host so publish and visibility timestamps come from one monotonic clock.
        probe = self.lab.graphers['grapher-b']
        if op == 'probe':
            node, folder, _ = self.services[probe['player']]
            self.launch('manifest-probe', node, f'cd {shlex.quote(folder)} && exec '
                        f'{shlex.quote(self.work[node] + "/bin/cluster_manifest_probe")} {shlex.quote(probe["archive"])} '
                        f'{service} >>manifest-probe.log 2>&1', 600)
            return ''
        if op == 'snapshot':
            self.collect()
            node, folder, _ = self.services[probe['player']]
            (OUT / 'manifest-probe.log').write_bytes(self.run(node, f'cat {shlex.quote(folder)}/manifest-probe.log', 10))
            return ''
        selected = roles.get(service, [service])
        if op == 'kill':
            for role in selected:
                self.stop(role)
        elif op == 'start':
            for role in selected:
                self.start(role)
        else:
            raise RuntimeError(f'unsupported control {op}')
        return ''

    def serve(self):
        self.listener = socket.socket(socket.AF_UNIX)
        if Path(self.socket_path).exists():
            Path(self.socket_path).unlink()
        self.listener.bind(self.socket_path)
        self.listener.listen(4)
        self.listener.settimeout(.5)
        self.shutdown = threading.Event()
        def loop():
            while not self.shutdown.is_set():
                try:
                    connection, _ = self.listener.accept()
                except socket.timeout:
                    continue
                with connection:
                    connection.settimeout(105)
                    try:
                        request = json.loads(connection.makefile().readline())
                        reply = {'output': self.control(request)}
                    except Exception as error:
                        reply = {'error': str(error)}
                    connection.sendall(json.dumps(reply).encode())
        self.thread = threading.Thread(target=loop)
        self.thread.start()

    def collect(self):
        if not self.work:
            return
        for row in self.lab.table:
            try:
                data = self.run(row['node'], f'cat {self.folder(row["node"])}/agent-results.jsonl', 10)
                (OUT / (row['node'] + '-agent.jsonl')).write_bytes(data)
            except Exception:
                pass
        for node in self.lab.nodes:
            try:
                data = self.run(node, f'cat {self.folder(node)}/sdk-agent.log', 10)
                (OUT / (node + '-sdk-agent.log')).write_bytes(data)
            except Exception:
                pass
        for role, (node, folder, _) in self.services.items():
            try:
                data = self.run(node, f'cat {shlex.quote(folder)}/{role}.log', 10)
                (OUT / f'{role}.log').write_bytes(data)
            except Exception:
                pass

    def close(self):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        try:
            if hasattr(self, 'shutdown'):
                self.shutdown.set()
                self.thread.join(timeout=65)
                self.listener.close()
                Path(self.socket_path).unlink(missing_ok=True)
            self.collect()
            for name in list(self.processes):
                self.stop(name)
            self.collect()
            if self.archive_owned:
                retained = f'{self.lab.archive}-{self.tag}'
                try:
                    self.run(self.lab.driver, f'mv {shlex.quote(self.lab.archive)} {shlex.quote(retained)}', 15)
                    print(f'Archive retained at {retained}', flush=True)
                except Exception as error:
                    print(f'FAIL preserve archive {error}', flush=True)
        finally:
            for node, unit, process in reversed(self.build_locks):
                process.stdin.close()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    try:
                        self.run(node, shlex.join(['systemctl', '--user', 'kill', '--signal=SIGKILL', unit]), 10)
                    except Exception:
                        pass
                    process.kill()
                    process.wait(timeout=5)


def main():
    parser = argparse.ArgumentParser(description='Multi-node lab harness; see the module docstring.')
    parser.add_argument('--config', required=True, help='lab description JSON')
    parser.add_argument('--plan', action='store_true', help='print the resolved placement and exit')
    parser.add_argument('--preflight-only', action='store_true')
    args = parser.parse_args()
    lab = Lab(args.config)
    if args.plan:
        print(json.dumps(lab.plan(), indent=2))
        return 0
    cluster = Cluster(lab)
    def interrupted(signum, frame):
        raise RuntimeError(f'driver interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    os.environ['TMPDIR'] = str(OUT)
    os.environ['PIP_CACHE_DIR'] = str(OUT / 'pip-cache')
    try:
        (OUT / 'stubs').mkdir(exist_ok=True)
        cluster.acquire_locks()
        cluster.preflight()
        if args.preflight_only:
            return 0
        python = ROOT / 'build/smoke-venv/bin/python'
        if not python.exists():
            subprocess.run(['python3', '-m', 'venv', str(python.parents[1])], timeout=30, check=True)
        subprocess.run([str(python), '-m', 'pip', 'install', '-r',
                        str(ROOT / 'tests/smoke/python/requirements.txt')], timeout=120, check=True,
                       stdout=open(OUT / 'pip.log', 'w'))
        subprocess.run([str(python), '-m', 'grpc_tools.protoc', '-I' + str(ROOT / 'proto'),
                        '--python_out=' + str(OUT / 'stubs'), '--grpc_python_out=' + str(OUT / 'stubs'),
                        'chronolog/v1/chronolog.proto', 'chronolog/internal/v1/internal.proto'],
                       timeout=30, check=True)
        for package in ('chronolog', 'chronolog/v1', 'chronolog/internal', 'chronolog/internal/v1'):
            (OUT / 'stubs' / package / '__init__.py').touch()
        sdk_package = ROOT / 'build/python/client/python/binding/package'
        if not list((sdk_package / 'chronolog').glob('_core*.so')):
            raise RuntimeError('build the Python SDK python_package target before a cluster run')
        cluster.stage()
        for role in cluster.services:
            if role.startswith('visor'):
                cluster.start(role)
        for role in cluster.services:
            if not role.startswith('visor'):
                cluster.start(role)
        cluster.serve()
        print(f'Logs {OUT}', flush=True)
        subprocess.run([str(python), str(ROOT / 'tools/lab-cluster/scenario.py'), '--config', str(lab.path)],
                       timeout=400, check=True,
                       env={**os.environ, 'CHRONOLOG_CLUSTER_SOCKET': cluster.socket_path,
                            'PYTHONPATH': str(sdk_package)})
        return 0
    except Exception as error:
        print(f'FAIL A7 {error}', flush=True)
        return 1
    finally:
        cluster.close()


if __name__ == '__main__':
    sys.exit(main())
