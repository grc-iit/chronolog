#!/usr/bin/env python3
import json
import selectors
import socket
import sys
from pathlib import Path

host, port = sys.argv[1].rsplit(':', 1)
targets = set(json.loads(sys.argv[2]))
blocked = Path(sys.argv[3])
selector = selectors.DefaultSelector()
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind((host, int(port)))
listener.listen(32)
listener.setblocking(False)
selector.register(listener, selectors.EVENT_READ)
pairs = {}
destinations = {}


def close(sock):
    peer = pairs.pop(sock, None)
    for connection in (sock, peer):
        if connection is not None:
            pairs.pop(connection, None)
            destinations.pop(connection, None)
            try:
                selector.unregister(connection)
            except KeyError:
                pass
            connection.close()


while True:
    if blocked.exists():
        for connection in list(pairs):
            if destinations.get(connection) in targets:
                close(connection)
    for key, _ in selector.select(.05):
        connection = key.fileobj
        if connection == listener:
            client, _ = listener.accept()
            if len(pairs) >= 128:
                client.close()
                continue
            try:
                client.settimeout(1)
                header = b''
                while b'\r\n\r\n' not in header and len(header) < 4096:
                    data = client.recv(1)
                    if not data:
                        raise OSError('CONNECT ended')
                    header += data
                method, target, _ = header.decode().split('\r\n', 1)[0].split(' ')
                if method != 'CONNECT' or (blocked.exists() and target in targets):
                    raise OSError('partitioned CONNECT')
                target_host, target_port = target.rsplit(':', 1)
                upstream = socket.create_connection((target_host, int(target_port)), timeout=1)
                client.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
                destinations[client] = destinations[upstream] = target
                client.settimeout(1)
                upstream.settimeout(1)
                pairs[client] = upstream
                pairs[upstream] = client
                selector.register(client, selectors.EVENT_READ)
                selector.register(upstream, selectors.EVENT_READ)
            except OSError:
                client.close()
        elif connection in pairs:
            try:
                data = connection.recv(65536)
                if not data:
                    close(connection)
                else:
                    pairs[connection].sendall(data)
            except OSError:
                close(connection)
