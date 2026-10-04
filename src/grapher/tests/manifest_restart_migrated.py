import ctypes
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'deploy/local'))
from chronolog_local.tiers import add_marker

grapher, driver = sys.argv[1:]
libc = ctypes.CDLL(None, use_errno=True)
with tempfile.TemporaryDirectory(prefix='tier2g-restart-') as scratch:
    root = Path(scratch)
    tiers = []
    for name, rank in [('local', 0), ('slow', 1)]:
        path = root / name
        path.mkdir()
        tiers.append(add_marker(dict(name=name, kind='posix', root=str(path), rank=rank), 'restart'))
    # The Grapher's own scrubber writes the validated mark that makes the seeded file eligible (I13.17).
    config = dict(deployment_id='restart', tiers=tiers, archive_root=str(root / 'local'),
                  manifest_writer='writer', archive_codec='proto', migrate_enabled=True,
                  tier_status_file=str(root / 'status.json'), tier_probe_interval_ms=100)
    # Reserve the listener by binding an ephemeral loopback port before starting.
    import socket
    import secrets
    for attempt in range(3):
        port = 10000 + secrets.randbelow(4400) * 5
        with socket.socket() as listener:
            try:
                listener.bind(('127.0.0.1', port))
                break
            except OSError:
                if attempt == 2:
                    raise
    config['internal_listen'] = config['self_endpoint'] = f'127.0.0.1:{port}'
    source = root / 'config.json'
    source.write_text(json.dumps(config))
    subprocess.run([driver, 'seed', str(source)], check=True, timeout=20)
    watcher = libc.inotify_init1(os.O_CLOEXEC | os.O_NONBLOCK)
    if watcher < 0 or libc.inotify_add_watch(watcher, os.fsencode(root), 0x80) < 0:
        raise OSError(ctypes.get_errno(), 'inotify setup')
    def start(log):
        output = open(root / log, 'w')
        process = subprocess.Popen([grapher, '--config', str(source)], stdout=subprocess.PIPE, stderr=output)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            ready, _, _ = select.select([process.stdout], [], [], max(0, deadline - time.monotonic()))
            if ready:
                line = process.stdout.readline()
                if b'grapher ready' in line:
                    return process, output
                if not line:
                    break
        process.kill()
        process.wait(timeout=10)
        output.close()
        raise RuntimeError('FAIL Grapher readiness ' + (root / log).read_text())
    process = None
    try:
        process, output = start('first.log')
        deadline = time.monotonic() + 30
        while True:
            status = root / 'status.json'
            if status.exists() and json.loads(status.read_text())['tiers'][1]['used_bytes'] > 0:
                break
            if time.monotonic() >= deadline or process.poll() is not None:
                raise RuntimeError('FAIL migration before restart')
            select.select([watcher], [], [], max(0, deadline - time.monotonic()))
            try:
                os.read(watcher, 65536)
            except BlockingIOError:
                pass
        subprocess.run([driver, 'verify', str(source)], check=True, timeout=20)
        process.kill()
        process.wait(timeout=10)
        output.close()
        process, output = start('restarted.log')
        subprocess.run([driver, 'verify', str(source)], check=True, timeout=20)
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=10)
        output.close()
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        os.close(watcher)
