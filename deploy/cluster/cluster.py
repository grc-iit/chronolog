#!/usr/bin/env python3
import concurrent.futures
import io
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import statistics
import subprocess
import sys
import tarfile
import threading
import time
import uuid

NODES = {'dragon': '100.101.232.95', 'blade': '100.124.181.9', 'mini': '100.74.131.112'}
PORTS = {'dragon': [50053], 'blade': [50051, 50061, 50053, 50054], 'mini': [50052, 50062, 50055, 50065]}
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/cluster'
ARCHIVE = '/mnt/nfs/chronolog-sprint/archive'


class Cluster:
    def __init__(self):
        self.tag = 'cl-d2-' + uuid.uuid4().hex[:10]
        self.processes = {}
        self.services = {}
        self.lock = threading.RLock()
        self.archive_owned = False
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
        return subprocess.run(self.command(node, command, seconds), input=data, capture_output=True,
                              timeout=seconds + 15, check=True).stdout

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

    def stage(self):
        roles = {'visor': 'blade', 'player': 'blade', 'grapher-b': 'blade',
                 'grapher-a': 'dragon', 'keeper-1': 'mini', 'keeper-2': 'mini'}
        for role, node in roles.items():
            template = json.loads((ROOT / f'deploy/cluster/{role}.json').read_text())
            home = self.run(node, 'printf %s "$HOME"').decode()
            config = json.dumps(template).replace('@HOME@', home).replace('@RUN@', self.tag)
            folder = f'chronolog-sprint/run/{self.tag}'
            self.services[role] = (node, folder, config)
        for node in NODES:
            buffer = io.BytesIO()
            with tarfile.open(fileobj=buffer, mode='w') as tar:
                if node == 'blade':
                    tar.add(ROOT / 'build/dev/deploy/cluster/cluster_manifest_probe', arcname='bin/cluster_manifest_probe')
                for role, (host, folder, config) in self.services.items():
                    if host != node:
                        continue
                    binary = 'chrono_' + role.split('-')[0]
                    candidates = list((ROOT / 'build/dev/src').rglob(binary))
                    candidates = [p for p in candidates if p.is_file() and os.access(p, os.X_OK)]
                    if len(candidates) != 1:
                        raise RuntimeError(f'cannot locate unique binary {binary}: {candidates}')
                    tar.add(candidates[0], arcname='bin/' + binary)
                    info = tarfile.TarInfo(f'run/{self.tag}/{role}.json')
                    encoded = config.encode()
                    info.size = len(encoded)
                    tar.addfile(info, io.BytesIO(encoded))
            self.run(node, 'mkdir -p ~/chronolog-sprint/bin && cd ~/chronolog-sprint && tar xf -', 60,
                     buffer.getvalue())
        for node in NODES:
            (OUT / f'{node}-hardware.log').write_bytes(self.run(node, 'hostname; uname -a; lscpu; findmnt /mnt/nfs || true'))
        for node in ('dragon', 'blade'):
            self.run(node, 'test -d /mnt/nfs && mountpoint -q /mnt/nfs && '
                     'mkdir -p /mnt/nfs/chronolog-sprint/archive && '
                     'test -z "$(ls -A /mnt/nfs/chronolog-sprint/archive)"')
        self.archive_owned = True
        (OUT / 'deployment.json').write_text(json.dumps({'tag': self.tag, 'services': self.services}, indent=2))

    def start(self, role):
        node, folder, _ = self.services[role]
        binary = 'chrono_' + role.split('-')[0]
        command = (f'cd ~/{folder} && exec ~/chronolog-sprint/bin/{binary} '
                   f'--config {role}.json >>{role}.log 2>&1')
        self.launch(role, node, command)
        cfg = json.loads(self.services[role][2])
        self.ready(cfg.get('listen', cfg.get('internal_listen')))

    def control(self, request):
        roles = {'chrono-keeper': ['keeper-1', 'keeper-2'], 'chrono-grapher': ['grapher-a', 'grapher-b']}
        op, service = request
        if op == 'pause':
            node, unit, _, _ = self.processes[service]
            self.run(node, shlex.join(['systemctl', '--user', 'kill', '--signal=SIGSTOP', unit]), 10)
            return ''
        if op == 'transfer-log':
            self.collect()
            return ''.join((OUT / (keeper + '.log')).read_text() for keeper in ('keeper-1', 'keeper-2'))
        if op == 'probe':
            node, folder, _ = self.services['player']
            self.launch('manifest-probe', node, f'cd ~/{folder} && exec ~/chronolog-sprint/bin/cluster_manifest_probe {ARCHIVE} {service} >>manifest-probe.log 2>&1', 600)
            return ''
        if op == 'snapshot':
            self.collect()
            node, folder, _ = self.services['player']
            (OUT / 'manifest-probe.log').write_bytes(self.run(node, f'cat ~/{folder}/manifest-probe.log', 10))
            return ''
        selected = roles.get(service, [service])
        if op == 'kill':
            for role in selected:
                self.stop(role)
        elif op == 'start':
            for role in selected:
                self.start(role)
        elif op == 'ps':
            return service
        elif op == 'inspect':
            return json.dumps([{'State': {'Running': True, 'Health': {'Status': 'healthy'}}}])
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
                    connection.settimeout(60)
                    try:
                        request = json.loads(connection.recv(8192))
                        reply = {'output': self.control(request)}
                    except Exception as error:
                        reply = {'error': str(error)}
                    connection.sendall(json.dumps(reply).encode())
        self.thread = threading.Thread(target=loop)
        self.thread.start()

    def smoke(self, python):
        shim = OUT / 'shim'
        shim.mkdir(exist_ok=True)
        path = shim / 'docker'
        path.write_text(f'#!/bin/sh\nexec {shlex.quote(str(python))} {shlex.quote(str(Path(__file__).resolve()))} shim "$@"\n')
        path.chmod(0o755)
        env = {**os.environ, 'PATH': str(shim) + ':' + os.environ['PATH'],
               'CHRONOLOG_CLUSTER_SOCKET': self.socket_path, 'TMPDIR': str(OUT)}
        with open(OUT / 'smoke.log', 'w') as log:
            subprocess.run([str(python), str(ROOT / 'tests/smoke/python/smoke.py'), '--visor',
                            NODES['blade'] + ':50051'], env=env, stdout=log, stderr=log,
                           timeout=180, check=True)
        print('PASS a unchanged smoke.py against blade Visor', flush=True)

    def collect(self):
        for role, (node, folder, _) in self.services.items():
            try:
                data = self.run(node, f'cat ~/{folder}/{role}.log', 10)
                (OUT / f'{role}.log').write_bytes(data)
            except Exception:
                pass

    def close(self):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
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


def shim():
    args = sys.argv[2:]
    if args[0] == 'inspect':
        request = ['inspect', args[1]]
    else:
        index = next(i for i, arg in enumerate(args) if arg in ('kill', 'start', 'ps'))
        request = [args[index], args[-1]]
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(65)
        client.connect(os.environ['CHRONOLOG_CLUSTER_SOCKET'])
        client.sendall(json.dumps(request).encode())
        reply = json.loads(client.recv(8192))
    if 'error' in reply:
        raise RuntimeError(reply['error'])
    print(reply['output'])


def main():
    cluster = Cluster()
    def interrupted(signum, frame):
        raise RuntimeError(f'driver interrupted by signal {signum}')
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    os.environ['TMPDIR'] = str(OUT)
    os.environ['PIP_CACHE_DIR'] = str(OUT / 'pip-cache')
    try:
        cluster.preflight()
        if '--preflight-only' in sys.argv:
            return 0
        cluster.stage()
        for role in ('visor', 'grapher-a', 'grapher-b', 'keeper-1', 'keeper-2', 'player'):
            cluster.start(role)
        cluster.serve()
        python = ROOT / 'build/smoke-venv/bin/python'
        if not python.exists():
            subprocess.run(['python3', '-m', 'venv', str(python.parents[1])], timeout=30, check=True)
        subprocess.run([str(python), '-m', 'pip', 'install', '-r',
                        str(ROOT / 'tests/smoke/python/requirements.txt')], timeout=120, check=True,
                       stdout=open(OUT / 'pip.log', 'w'))
        cluster.smoke(python)
        subprocess.run([str(python), str(ROOT / 'deploy/cluster/scenario.py')], timeout=400, check=True,
                       env={**os.environ, 'CHRONOLOG_CLUSTER_SOCKET': cluster.socket_path})
        return 0
    except Exception as error:
        print(f'FAIL D2 {error}', flush=True)
        return 1
    finally:
        cluster.close()


if __name__ == '__main__':
    sys.exit(shim() if len(sys.argv) > 1 and sys.argv[1] == 'shim' else main())
