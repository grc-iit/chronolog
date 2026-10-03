#!/usr/bin/env python3
import base64
import json
from pathlib import Path
import sys
import time
import uuid

import grpc
from append_retry import append
sys.path.insert(0, 'stubs')
from chronolog.v1 import chronolog_pb2 as pb, chronolog_pb2_grpc as rpc


def main(request):
    state_path = Path('agent-' + request['slot'] + '.json')
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    channels = []
    def connect(endpoint):
        channel = grpc.insecure_channel(endpoint)
        channels.append(channel)
        return channel
    try:
        if request['op'] in ('acquire', 'reacquire'):
            acquire = pb.AcquireRequest(story_id=request['story'], writer_identity=request['identity'],
                acquire_request_id=uuid.uuid4().hex, preferred_keeper_process_id=request['preferred'],
                lease_duration_ns=3_600_000_000_000)
            if request['op'] == 'reacquire':
                previous = pb.AcquireResponse.FromString(base64.b64decode(state['acquired']))
                acquire.takeover = request.get('takeover', False)
                acquire.expected_prior_incarnation = previous.incarnation
            deadline = time.monotonic() + 30
            last = None
            while time.monotonic() < deadline:
                for endpoint in request['catalogs']:
                    try:
                        acquired = rpc.CatalogStub(connect(endpoint)).Acquire(acquire, timeout=3)
                        assert acquired.status.code == 0, acquired.status
                        assert acquired.lease.duration_ns == 3_600_000_000_000 and acquired.lease.remaining_ns > 0
                        if request['op'] == 'reacquire':
                            assert acquired.writer_id == previous.writer_id and acquired.incarnation > previous.incarnation
                        state = dict(acquired=base64.b64encode(acquired.SerializeToString()).decode(), sequence=0,
                                     deadline=time.monotonic() + acquired.lease.remaining_ns / 1e9)
                        state_path.write_text(json.dumps(state))
                        result = dict(acquired=state['acquired'], request_id=acquire.acquire_request_id,
                            node=request['node'], preferred=request['preferred'], assigned=acquired.assigned_keeper.process_id,
                            keeper_preference=pb.KeeperPreferenceResult.Name(acquired.keeper_preference))
                        with open('agent-results.jsonl', 'a') as log:
                            log.write(json.dumps(result) + '\n')
                        return result
                    except grpc.RpcError as error:
                        if error.code() != grpc.StatusCode.UNAVAILABLE:
                            raise
                        last = str(error)
                time.sleep(.1)
            raise RuntimeError('acquire stayed unavailable: ' + str(last))
        acquired = pb.AcquireResponse.FromString(base64.b64decode(state['acquired']))
        route = None
        for endpoint in request['catalogs']:
            try:
                story = rpc.CatalogStub(connect(endpoint)).GetStory(
                    pb.GetStoryRequest(story_id=acquired.story_id), timeout=3)
                assert story.status.code == 0, story
                route = story.story.route
                break
            except grpc.RpcError as error:
                if error.code() != grpc.StatusCode.UNAVAILABLE:
                    raise
        assert route is not None, 'no Catalog could refresh the Route'
        assigned = next((k for k in route.keepers if k.process_id == acquired.assigned_keeper.process_id), None)
        assert assigned is not None, 'removed owner needs explicit conditional re-acquire'
        acquired.route.CopyFrom(route)
        acquired.assigned_keeper.CopyFrom(assigned)
        state['acquired'] = base64.b64encode(acquired.SerializeToString()).decode()
        journal = rpc.JournalStub(connect(acquired.assigned_keeper.endpoint))
        events, latency = [], []
        for sequence in range(state['sequence'] + 1, state['sequence'] + request['count'] + 1):
            assert time.monotonic() < state['deadline'], 'append outside finite grant'
            payload = f"{request['node']}:{request['slot']}:{acquired.incarnation}:{sequence}".encode()
            item = pb.AppendItem(writer_id=acquired.writer_id, incarnation=acquired.incarnation, sequence=sequence,
                physical=pb.TimeReading(physical_ns=time.time_ns(), status=pb.CLOCK_STATUS_UNSYNCED),
                envelope=pb.Envelope(payload=payload))
            started = time.perf_counter_ns()
            response = append(journal, pb.AppendRequest(story_id=acquired.story_id, epoch=acquired.route.epoch,
                durability=pb.DURABILITY_DURABLE, batch_id=sequence, items=[item]), grant_deadline=state['deadline'])
            latency.append((time.perf_counter_ns() - started) / 1e6)
            assert response.batch_id == sequence and len(response.results) == 1, response
            result = response.results[0]
            assert result.status.code == 0 and result.achieved_durability == pb.DURABILITY_DURABLE, result
            key = (acquired.story_id, acquired.writer_id, acquired.incarnation, sequence)
            assert (result.id.story_id, result.id.writer_id, result.id.incarnation, result.id.sequence) == key
            events.append([*key, result.assigned_hlc.physical_ns, result.assigned_hlc.logical, payload.hex()])
        state['sequence'] += request['count']
        state_path.write_text(json.dumps(state))
        return dict(events=events, latency_ms=latency)
    finally:
        for channel in channels:
            channel.close()


if __name__ == '__main__':
    print(json.dumps(main(json.load(sys.stdin))))
