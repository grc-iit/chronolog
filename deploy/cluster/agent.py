#!/usr/bin/env python3
import json
from pathlib import Path
import socket
import sys
import time

import chronolog as cl


class Agent:
    def __init__(self):
        self.slots = {}

    def request(self, request):
        slot = request['slot']
        previous = self.slots.get(slot)
        if request['op'] in ('acquire', 'reacquire'):
            client = previous['client'] if previous else cl.connect(','.join(request['catalogs']),
                timeout=10, max_retries=500, retry_backoff=.02)
            request_id = client.new_acquire_request_id()
            prior = previous['writer'].incarnation if request['op'] == 'reacquire' else None
            writer = client.acquire(request['story'], request['identity'], options=cl.AcquireOptions(
                acquire_request_id=request_id, preferred_keeper_process_id=request['preferred'],
                lease_duration_ns=3_600_000_000_000, expected_prior_incarnation=prior,
                takeover=request.get('takeover', False)), timeout=30)
            acquired = writer.acquisition
            assert acquired.lease.duration_ns == 3_600_000_000_000 and acquired.lease.remaining_ns > 0
            if prior is not None:
                assert writer.writer_id == previous['writer'].writer_id and writer.incarnation > prior
            self.slots[slot] = dict(client=client, writer=writer, sequence=0)
            result = dict(request_id=request_id, node=request['node'], preferred=request['preferred'],
                assigned=acquired.assigned_keeper.process_id,
                keeper_preference='KEEPER_PREFERENCE_RESULT_' + acquired.keeper_preference.name)
            with open('agent-results.jsonl', 'a') as log:
                log.write(json.dumps(result) + '\n')
            return result
        assert request['op'] == 'append', request
        writer = previous['writer']
        events, latency = [], []
        for sequence in range(previous['sequence'] + 1, previous['sequence'] + request['count'] + 1):
            payload = f"{request['node']}:{slot}:{writer.incarnation}:{sequence}".encode()
            started = time.perf_counter_ns()
            result = writer.append(payload, durability=cl.Durability.DURABLE,
                physical=cl.TimeReading(time.time_ns(), status=1), timeout=10)
            latency.append((time.perf_counter_ns() - started) / 1e6)
            key = (writer.story_id, writer.writer_id, writer.incarnation, sequence)
            event = result.event_id
            assert (event.story_id, event.writer_id, event.incarnation, event.sequence) == key
            assert result.durability == cl.Durability.DURABLE, result
            events.append([*key, result.hlc.physical_ns, result.hlc.logical, payload.hex()])
            previous['sequence'] = sequence
        return dict(events=events, latency_ms=latency)

    def close(self):
        for state in self.slots.values():
            try:
                state['writer'].release(timeout=5)
            except cl.Error:
                pass


def serve(path):
    agent = Agent()
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(path)
        listener.listen(4)
        listener.settimeout(1)
        deadline = time.monotonic() + 820
        try:
            while time.monotonic() < deadline:
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                with connection:
                    connection.settimeout(90)
                    try:
                        reply = {'result': agent.request(json.loads(connection.makefile().readline()))}
                    except Exception as error:
                        reply = {'error': f'{type(error).__name__}: {error}'}
                    connection.sendall((json.dumps(reply) + '\n').encode())
        finally:
            agent.close()
            Path(path).unlink(missing_ok=True)


def main(request, path='agent.sock'):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(90)
        connection.connect(path)
        connection.sendall((json.dumps(request) + '\n').encode())
        reply = json.loads(connection.makefile().readline())
    if 'error' in reply:
        raise RuntimeError(reply['error'])
    return reply['result']


if __name__ == '__main__':
    if '--serve' in sys.argv:
        serve(sys.argv[2] if len(sys.argv) > 2 else 'agent.sock')
    else:
        print(json.dumps(main(json.load(sys.stdin))))
