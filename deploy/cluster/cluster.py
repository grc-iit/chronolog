#!/usr/bin/env python3
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

from topology import NODES, TABLE, PORTS, ARCHIVE, configs
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/cluster'


class Cluster:
    def __init__(self):
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
        if node == 'dragon':
            return scoped
        return ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', node, shlex.join(scoped)]

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

    def preflight(self):
        for row in TABLE:
            node = row['node']
            try:
                self.run(node, 'test -d /data/chronolog-sprint' if node == 'mini' else
                         'test -d /mnt/nfs && mountpoint -q /mnt/nfs', 10)
            except Exception as error:
                raise RuntimeError(f'shared archive parent unavailable on {node}') from error
        server = 'import socket,time; ports=PORTS; sockets=[]\nfor p in ports:\n s=socket.socket(); s.bind(("IP",p)); s.listen(); sockets.append(s)\ntime.sleep(90)'
        try:
            for node, ports in PORTS.items():
                code = server.replace('PORTS', repr(ports)).replace('IP', NODES[node])
                self.launch('probe-' + node, node, 'exec python3 -c ' + shlex.quote(code), 90)
            time.sleep(2)
            for source in NODES:
                for target, ports in PORTS.items():
                    if source == target:
                        continue
                    for port in ports:
                        code = f'import socket; socket.create_connection(({NODES[target]!r},{port}),timeout=3).close()'
                        try:
                            self.run(source, 'exec python3 -c ' + shlex.quote(code), 6)
                        except Exception as error:
                            raise RuntimeError(f'reachability {source}->{target}:{port}: {error}') from error
            print('PASS reachability every pair on service ports', flush=True)
        finally:
            for node in NODES:
                self.stop('probe-' + node)

    def acquire_locks(self):
        if os.environ.get('RBUILD_HELD') != 'build':
            raise RuntimeError('run through rbuild with the dragon build lock')
        for node in ('mini', 'blade'):
            unit = f'{self.tag}-build-lock-{node}.scope'
            process = subprocess.Popen(self.command(node,
                'exec 9>~/chronolog-sprint/build.lock; flock 9; echo LOCKED; cat >/dev/null', 900, unit),
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
        print('PASS build locks held on dragon mini blade', flush=True)

    def stage(self):
        homes = {node: self.run(node, 'printf %s "$HOME"').decode() for node in NODES}
        for role, (node, config) in configs(homes, self.tag).items():
            self.services[role] = (node, f'chronolog-sprint/run/{self.tag}', json.dumps(config))
        for node in NODES:
            name = 'stage-' + node
            unit = f'{self.tag}-{name}.scope'
            log = open(OUT / f'{name}.transport.log', 'ab')
            receiver = subprocess.Popen(self.command(node,
                'mkdir -p ~/chronolog-sprint/bin && cd ~/chronolog-sprint && tar xf -', 60, unit),
                stdin=subprocess.PIPE, stdout=log, stderr=log)
            self.processes[name] = (node, unit, receiver, log)
            try:
                with tarfile.open(fileobj=receiver.stdin, mode='w|') as tar:
                    if node == 'blade':
                        tar.add(ROOT / 'build/dev/deploy/cluster/cluster_manifest_probe', arcname='bin/cluster_manifest_probe')
                    tar.add(ROOT / 'deploy/cluster/agent.py', arcname=f'run/{self.tag}/agent.py')
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
        for node in NODES:
            self.run(node, f'cd ~/chronolog-sprint/run/{self.tag} && '
                     f'python3 -m venv ~/chronolog-sprint/build/cluster/{self.tag}/venv && '
                     f'~/chronolog-sprint/build/cluster/{self.tag}/venv/bin/pip install '
                     '-r requirements.txt >pip.log 2>&1', 120)
            (OUT / f'{node}-hardware.log').write_bytes(self.run(node, 'hostname; uname -a; lscpu; findmnt /mnt/nfs || true'))
        for row in TABLE:
            if not row['grapher']:
                continue
            node = row['node']
            self.run(node, 'test -d /mnt/nfs && mountpoint -q /mnt/nfs && '
                     'mkdir -p /mnt/nfs/chronolog-sprint/archive && '
                     'test -z "$(ls -A /mnt/nfs/chronolog-sprint/archive)"')
        self.archive_owned = True
        marker = f'{ARCHIVE}/{self.tag}.probe'
        self.run('dragon', f'printf %s {self.tag} >{marker}', 10)
        for row in TABLE:
            self.run(row['node'], f'test "$(cat {row["archive"]}/{self.tag}.probe)" = {self.tag}', 10)
        self.run('dragon', f'rm {marker}', 10)
        (OUT / 'deployment.json').write_text(json.dumps({'tag': self.tag, 'services': self.services, 'topology': TABLE}, indent=2))

    def start(self, role):
        node, folder, _ = self.services[role]
        binary = 'chrono_' + role.split('-')[0]
        command = (f'cd ~/{folder} && exec ~/chronolog-sprint/bin/{binary} '
                   f'--config {role}.json >>{role}.log 2>&1')
        self.launch(role, node, command)
        cfg = json.loads(self.services[role][2])
        self.ready(cfg.get('listen', cfg.get('internal_listen')))

    def control(self, request):
        roles = {'chrono-keeper': [r['keeper'] for r in TABLE], 'chrono-grapher': ['grapher-a', 'grapher-b']}
        op, service = request
        if op == 'agent':
            node = service['node']
            data = json.dumps(service).encode()
            return self.run(node, f'cd ~/chronolog-sprint/run/{self.tag} && '
                            f'exec ~/chronolog-sprint/build/cluster/{self.tag}/venv/bin/python agent.py', 90, data).decode()
        if op in ('pause', 'resume'):
            node, unit, _, _ = self.processes[service]
            sig = 'SIGSTOP' if op == 'pause' else 'SIGCONT'
            self.run(node, shlex.join(['systemctl', '--user', 'kill', '--signal=' + sig, unit]), 10)
            return ''
        if op == 'transfer-log':
            self.collect()
            return ''.join((OUT / (keeper + '.log')).read_text() for keeper in (r['keeper'] for r in TABLE))
        if op == 'probe':
            node, folder, _ = self.services['player-2']
            self.launch('manifest-probe', node, f'cd ~/{folder} && exec ~/chronolog-sprint/bin/cluster_manifest_probe {ARCHIVE} {service} >>manifest-probe.log 2>&1', 600)
            return ''
        if op == 'snapshot':
            self.collect()
            node, folder, _ = self.services['player-2']
            (OUT / 'manifest-probe.log').write_bytes(self.run(node, f'cat ~/{folder}/manifest-probe.log', 10))
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
        for row in TABLE:
            try:
                data = self.run(row['node'], f'cat ~/chronolog-sprint/run/{self.tag}/agent-results.jsonl', 10)
                (OUT / (row['node'] + '-agent.jsonl')).write_bytes(data)
            except Exception:
                pass
        for role, (node, folder, _) in self.services.items():
            try:
                data = self.run(node, f'cat ~/{folder}/{role}.log', 10)
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
                try:
                    self.run('dragon', f'mv {ARCHIVE} /mnt/nfs/chronolog-sprint/archive-{self.tag}', 15)
                    print(f'Archive retained at /mnt/nfs/chronolog-sprint/archive-{self.tag}', flush=True)
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
    cluster = Cluster()
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
        if '--preflight-only' in sys.argv:
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
        cluster.stage()
        for role in cluster.services:
            if role.startswith('visor'):
                cluster.start(role)
        for role in cluster.services:
            if not role.startswith('visor'):
                cluster.start(role)
        cluster.serve()
        print(f'Logs {OUT}', flush=True)
        subprocess.run([str(python), str(ROOT / 'deploy/cluster/scenario.py')], timeout=400, check=True,
                       env={**os.environ, 'CHRONOLOG_CLUSTER_SOCKET': cluster.socket_path})
        return 0
    except Exception as error:
        print(f'FAIL A7 {error}', flush=True)
        return 1
    finally:
        cluster.close()


if __name__ == '__main__':
    sys.exit(main())
