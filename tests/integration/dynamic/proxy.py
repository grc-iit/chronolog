#!/usr/bin/env python3
import json
import re
import socket
import sys
import threading
import time
from pathlib import Path

host, port = sys.argv[1].rsplit(':', 1)
targets = set(json.loads(sys.argv[2]))
blocked = Path(sys.argv[3])
MAX_CONNECTIONS = 128
lock = threading.Lock()
connections = {}


def log(*message):
    print(f'{time.time():.3f}', *message, file=sys.stderr, flush=True)


def candidates(target):
    # gRPC hands a multi-replica target to the proxy whole, as "[a:p,b:p,c:p]:443". The proxy plays pick_first over it.
    wrapped = re.fullmatch(r'\[(.*)\]:\d+', target)
    return (wrapped.group(1) if wrapped else target).split(',')


def read_header(client):
    header = b''
    while b'\r\n\r\n' not in header and len(header) < 4096:
        data = client.recv(4096)
        if not data:
            raise OSError('CONNECT ended')
        header += data
    return header


def close(pair):
    with lock:
        destination = connections.pop(pair, None)
    if destination:
        log('close', destination)
    for sock in pair:
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()


def pump(source, sink, pair):
    # An end of either direction closes both, so a dead upstream is a dead connection for the Keeper too.
    try:
        while data := source.recv(65536):
            sink.sendall(data)
    except OSError:
        pass
    close(pair)


def serve(client):
    try:
        client.settimeout(5)
        method, target, _ = read_header(client).decode().split('\r\n', 1)[0].split(' ')
        found = candidates(target)
        if method != 'CONNECT' or (blocked.exists() and any(c in targets for c in found)):
            raise OSError('partitioned CONNECT ' + target)
        upstream = destination = None
        for candidate in found:
            target_host, target_port = candidate.rsplit(':', 1)
            try:
                upstream = socket.create_connection((target_host, int(target_port)), timeout=1)
                destination = candidate
                break
            except (OSError, ValueError):
                continue
        if upstream is None:
            raise OSError('no replica reachable ' + target)
    except (OSError, ValueError) as error:
        log('refused', error)
        client.close()
        return
    client.settimeout(None)
    upstream.settimeout(None)
    pair = (client, upstream)
    with lock:
        partitioned = blocked.exists() and destination in targets
        if not partitioned:
            connections[pair] = destination
    if partitioned:
        log('refused', 'partitioned CONNECT ' + target)
        close(pair)
        return
    try:
        client.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
    except OSError:
        close(pair)
        return
    log('connect', target, '->', destination)
    threading.Thread(target=pump, args=(upstream, client, pair), daemon=True).start()
    pump(client, upstream, pair)


def partition():
    active = False
    while True:
        time.sleep(.05)
        if blocked.exists():
            with lock:
                cut = [pair for pair, destination in connections.items() if destination in targets]
            for pair in cut:
                close(pair)
            if not active:
                log('partition applied')
            active = True
        else:
            active = False


listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind((host, int(port)))
listener.listen(128)
threading.Thread(target=partition, daemon=True).start()
while True:
    accepted, _ = listener.accept()
    with lock:
        full = len(connections) >= MAX_CONNECTIONS
    if full:
        accepted.close()
    else:
        threading.Thread(target=serve, args=(accepted,), daemon=True).start()
