#!/usr/bin/env python3
import base64
import concurrent.futures
import importlib
import json
import os
from pathlib import Path
import re
import socket
import statistics
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(os.environ['CHRONOLOG_CLUSTER_OUT'])
from topology import TABLE
from append_retry import append
ARCHIVE = Path('/mnt/nfs/chronolog-sprint/archive')
sys.path.insert(0, str(ROOT / 'tests/smoke/python'))
from smoke import Smoke, hlc_key


def control(op, service):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(90)
        client.connect(os.environ['CHRONOLOG_CLUSTER_SOCKET'])
        client.sendall((json.dumps([op, service]) + "\n").encode())
        reply = json.loads(client.makefile().read())
    if 'error' in reply:
        raise RuntimeError(reply['error'])
    return reply['output']


class Scenario(Smoke):
    # The Visor's configured acquisition_lease_max_ns. This raw driver never renews: it takes the maximum lease
    # and append_writer asserts every hold stays inside the returned grant.
    MAX_LEASE_NS = 3_600_000_000_000

    def __init__(self, stubs):
        super().__init__(stubs, TABLE[0]['ip'] + ':50051', 10, 'docker', 'cluster', [])
        self.stories = []
        self.writers = {}
        self.expected = {}
        self.sequences = {}

    def settled_chunks(self, story):
        logs = control('transfer-log', '')
        pattern = rf'archive_settled chunk=\S+ story={story} start=(\d+):(\d+) end=(\d+):(\d+)'
        return sorted(((int(m[1]), int(m[2])), (int(m[3]), int(m[4]))) for m in re.finditer(pattern, logs))

    def append(self, acquired, sequence, durability, floor=None):
        self.hold(acquired)
        pb = self.pb
        item = pb.AppendItem(writer_id=acquired.writer_id, incarnation=acquired.incarnation, sequence=sequence,
            physical=pb.TimeReading(physical_ns=time.time_ns(), status=pb.CLOCK_STATUS_UNSYNCED),
            envelope=pb.Envelope(payload=f'event-{sequence}'.encode()))
        if floor is not None:
            item.causal_floor.CopyFrom(floor)
        response = append(self.journal, pb.AppendRequest(story_id=acquired.story_id, epoch=acquired.route.epoch,
            items=[item], durability=durability, batch_id=sequence), seconds=self.timeout,
            grant_deadline=self.holds[(acquired.story_id, acquired.writer_id, acquired.incarnation)])
        return response.results[0]

    def restart(self, service):
        roles = ([r['keeper'] for r in TABLE] if service == 'chrono-keeper'
                 else [r['grapher'] for r in TABLE if r['grapher']])
        for role in roles:
            control('kill', role)
        for role in roles:
            control('start', role)

    def call(self, method, request, seconds=30):
        deadline = time.monotonic() + seconds
        last = None
        while time.monotonic() < deadline:
            for row in TABLE:
                try:
                    return getattr(self.rpc.CatalogStub(self.connect(row['ip'] + ':50051')), method)(
                        request, timeout=min(3, max(.1, deadline - time.monotonic())))
                except self.grpc.RpcError as error:
                    if error.code() != self.grpc.StatusCode.UNAVAILABLE:
                        raise
                    last = str(error)
            time.sleep(.1)
        raise RuntimeError(f'{method} stayed unavailable: {last}')

    def acquire(self, story, identity, prior=None, lease_ns=None, preferred=None):
        request = self.pb.AcquireRequest(story_id=story, writer_identity=identity,
            acquire_request_id=uuid.uuid4().hex, lease_duration_ns=lease_ns or self.MAX_LEASE_NS)
        if prior is not None:
            request.takeover = True
            request.expected_prior_incarnation = prior
        if preferred is not None:
            request.preferred_keeper_process_id = preferred
        acquired = self.call('Acquire', request)
        if acquired.status.code == 0:
            self.holds[(acquired.story_id, acquired.writer_id, acquired.incarnation)] = (
                time.monotonic() + acquired.lease.remaining_ns / 1e9)
        return acquired

    def membership(self):
        from chronolog.internal.v1 import internal_pb2 as ipb, internal_pb2_grpc as irpc
        for row in TABLE:
            try:
                result = irpc.ClusterStub(self.connect(row['ip'] + ':50061')).ListMembers(
                    ipb.ListMembersRequest(), timeout=3)
                if result.status.code == 0:
                    return result
            except self.grpc.RpcError as error:
                if error.code() != self.grpc.StatusCode.UNAVAILABLE:
                    raise
        raise RuntimeError('ListMembers unavailable')

    def wait_membership(self, predicate):
        deadline = time.monotonic() + 30
        last = None
        while time.monotonic() < deadline:
            try:
                last = self.membership()
                if predicate(last):
                    return last
            except RuntimeError:
                pass
            time.sleep(.1)
        raise RuntimeError(f'membership did not converge: {last}')

    def agent(self, row, slot, op, **options):
        return json.loads(control('agent', dict(node=row['node'], slot=slot, op=op,
            catalogs=[r['ip'] + ':50051' for r in TABLE], **options)))

    def agent_append(self, row, slot, count):
        response = self.agent(row, slot, 'append', count=count)
        for event in response['events']:
            self.expected[event[0]].append((*event[:6], bytes.fromhex(event[6])))
        return response['latency_ms']

    def request(self, story):
        events = self.expected[story]
        start = min((e[4], e[5]) for e in events)
        end = max((e[4], e[5]) for e in events)
        return self.pb.ReadRequest(story_id=story, hlc=self.pb.HlcRange(
            start=self.pb.Hlc(physical_ns=start[0], logical=start[1]),
            end=self.pb.Hlc(physical_ns=end[0], logical=end[1] + 1)))

    def every_player(self, story):
        expected = sorted(self.expected[story], key=lambda e: (e[4], e[5], e[1], e[2], e[3]))
        for row in TABLE:
            self.replay = self.rpc.ReplayStub(self.connect(row['ip'] + ':50054'))
            self.read_complete(self.request(story), expected)

    def a7(self):
        self.wait_membership(lambda m: all(any(x.process.process_id == r['keeper'] and
            any(i.granted for i in x.instances) for x in m.members) for r in TABLE))
        chronicle = 'a7-' + uuid.uuid4().hex[:8]
        created = self.call('CreateChronicle', self.pb.CreateChronicleRequest(name=chronicle))
        assert created.status.code == 0, created
        created = self.call('CreateStory', self.pb.CreateStoryRequest(chronicle=chronicle, name='locality'))
        assert created.status.code == 0, created
        story = created.story.story_id
        self.expected[story] = []
        acquisitions, latency = {}, []
        for index, row in enumerate(TABLE):
            for slot, preferred in (('local', row['keeper']), ('remote', TABLE[(index + 1) % 3]['keeper'])):
                result = self.agent(row, slot, 'acquire', story=story,
                    identity=chronicle + '-' + row['node'] + '-' + slot, preferred=preferred)
                assert result['assigned'] == preferred, result
                assert result['keeper_preference'] == 'KEEPER_PREFERENCE_RESULT_HONORED', result
                acquisitions[(row['node'], slot)] = result
                samples = self.agent_append(row, slot, 100)
                latency.append(dict(node=row['node'], placement=slot, preferred=preferred,
                    assigned=result['assigned'], keeper_preference=result['keeper_preference'], samples=len(samples),
                    p50_ms=statistics.median(samples), p99_ms=sorted(samples)[int(.99 * (len(samples) - 1))]))
        self.every_player(story)
        result = dict(build='dev Debug', commit=os.environ.get('CHRONOLOG_CLUSTER_COMMIT', 'unknown'), rows=latency)
        (OUT / 'append-latency.json').write_text(json.dumps(result, indent=2) + '\n')
        print('node local_p50_ms local_p99_ms remote_p50_ms remote_p99_ms', flush=True)
        for row in TABLE:
            local, remote = [x for x in latency if x['node'] == row['node']]
            print(f"{row['node']} {local['p50_ms']:.3f} {local['p99_ms']:.3f} {remote['p50_ms']:.3f} {remote['p99_ms']:.3f}", flush=True)
        print('PASS A7a every node HONORED local and forced remote DURABLE writes all Players complete', flush=True)
        _, metadata = self.catalog.GetStory.with_call(self.pb.GetStoryRequest(story_id=story), timeout=10)
        leader = int(dict(metadata.initial_metadata())['chronolog-raft-leader'])
        killed = next(r for r in TABLE if r['replica'] == leader)
        control('kill', killed['visor'])
        try:
            response = self.call('CreateStory', self.pb.CreateStoryRequest(chronicle=chronicle, name='after-election'))
            assert response.status.code == 0, response
            for row in TABLE:
                self.agent_append(row, 'local', 10)
            self.every_player(story)
            _, metadata = self.rpc.CatalogStub(self.connect(next(r['ip'] for r in TABLE if r != killed) + ':50051')).GetStory.with_call(
                self.pb.GetStoryRequest(story_id=story), timeout=10)
            replacement = int(dict(metadata.initial_metadata())['chronolog-raft-leader'])
            assert replacement in [r['replica'] for r in TABLE if r != killed], replacement
            print(f'PASS A7b leader {leader} killed elected {replacement} catalog writes DURABLE appends all Players complete no loss', flush=True)
        finally:
            control('start', killed['visor'])
        failed = TABLE[0]
        for row in TABLE:
            if row['grapher']:
                control('pause', row['grapher'])
        self.agent_append(failed, 'local', 10)
        failed_instance = next(m.process.instance for m in self.membership().members
                               if m.process.process_id == failed['keeper'])
        control('kill', failed['keeper'])
        try:
            removed = self.wait_membership(lambda m: any(r.story_id == story and
                failed['keeper'] not in [k.process_id for k in r.route.keepers] for r in m.routes))
            for row in TABLE:
                self.replay = self.rpc.ReplayStub(self.connect(row['ip'] + ':50054'))
                _, completion = self.read(self.request(story))
                assert not completion.complete and completion.reason in (
                    self.pb.INCOMPLETE_REASON_SOURCE_FAILED, self.pb.INCOMPLETE_REASON_LAGGING_WRITERS), completion
            for row in TABLE:
                for slot in ('local', 'remote'):
                    previous = acquisitions[(row['node'], slot)]
                    if previous['assigned'] != failed['keeper']:
                        continue
                    stale = self.agent(row, slot, 'reacquire', story=story,
                        identity=chronicle + '-' + row['node'] + '-' + slot, preferred=failed['keeper'])
                    assert stale['keeper_preference'] == 'KEEPER_PREFERENCE_RESULT_NOT_IN_ROUTE', stale
                    assert stale['assigned'] != failed['keeper'], stale
                    assert stale['request_id'] != previous['request_id'], stale
            survivor = TABLE[1]
            retained = self.agent(survivor, 'local', 'reacquire', story=story,
                identity=chronicle + '-' + survivor['node'] + '-local', preferred=TABLE[2]['keeper'], takeover=True)
            assert retained['keeper_preference'] == 'KEEPER_PREFERENCE_RESULT_RETAINED', retained
            assert retained['assigned'] == survivor['keeper'], retained
            print('PASS A7c outage every Player honest incomplete fresh conditional CAS NOT_IN_ROUTE remap and RETAINED surviving affinity', flush=True)
        finally:
            control('start', failed['keeper'])
            for row in TABLE:
                if row['grapher']:
                    control('resume', row['grapher'])
        self.wait_membership(lambda m: any(x.process.process_id == failed['keeper'] and
            x.process.instance != failed_instance and any(i.granted for i in x.instances
            if i.instance == x.process.instance) for x in m.members))
        # Rejoin is explicit after dynamic removal; registration alone cannot change a committed Route.
        from chronolog.internal.v1 import internal_pb2 as ipb, internal_pb2_grpc as irpc
        joined = irpc.ClusterStub(self.connect(TABLE[1]['ip'] + ':50061')).JoinKeeper(
            ipb.JoinKeeperRequest(process_id=failed['keeper']), timeout=10)
        assert joined.status.code == 0, joined
        self.wait_membership(lambda m: any(r.story_id == story and len(r.route.keepers) == 3 for r in m.routes))
        for row in (failed, survivor):
            self.agent_append(row, 'local', 10)
        self.every_player(story)
        print('PASS A7c recovered Keeper WAL and archive all acknowledged DURABLE ids HLCs payloads complete on every Player', flush=True)

    def setup(self):
        pb = self.pb
        chronicle = 'cluster-' + uuid.uuid4().hex[:8]
        response = self.call('CreateChronicle', pb.CreateChronicleRequest(name=chronicle))
        assert response.status.code == 0, response.status
        for i in range(10):
            response = self.call('CreateStory', pb.CreateStoryRequest(chronicle=chronicle, name=f's{i}'))
            assert response.status.code == 0, response.status
            story = response.story.story_id
            self.stories.append(story)
            self.expected[story] = []
            self.writers[story] = []
            for writer, row in enumerate(TABLE):
                identity = f'{chronicle}-{i}-{writer}'
                acquired = self.acquire(story, identity, lease_ns=self.MAX_LEASE_NS, preferred=row['keeper'])
                assert acquired.status.code == 0, acquired.status
                assert acquired.lease.duration_ns == self.MAX_LEASE_NS, acquired.lease
                assert acquired.route.grapher == [r['ip'] + ':50053' for r in TABLE if r['grapher']][story % 2]
                self.writers[story].append((identity, acquired))
                self.sequences[(story, identity)] = 0
            assert len({a.assigned_keeper.process_id for _, a in self.writers[story]}) == 3
        assert len({a.route.grapher for writers in self.writers.values() for _, a in writers}) == 2
        control('probe', ' '.join(map(str, self.stories)))
        time.sleep(.3)

    def append_writer(self, story, identity, acquired, count):
        pb = self.pb
        self.hold(acquired)
        journal = self.rpc.JournalStub(self.connect(acquired.assigned_keeper.endpoint))
        expected = []
        first = self.sequences[(story, identity)] + 1
        for offset in range(0, count, 100):
            items = []
            for sequence in range(first + offset, min(first + count, first + offset + 100)):
                items.append(pb.AppendItem(writer_id=acquired.writer_id, incarnation=acquired.incarnation,
                    sequence=sequence, physical=pb.TimeReading(physical_ns=time.time_ns(), status=pb.CLOCK_STATUS_UNSYNCED),
                    envelope=pb.Envelope(payload=f'{identity}-{sequence}'.encode())))
            response = append(journal, pb.AppendRequest(story_id=story, epoch=acquired.route.epoch,
                durability=pb.DURABILITY_DURABLE, batch_id=first+offset, items=items),
                grant_deadline=self.holds[(story, acquired.writer_id, acquired.incarnation)])
            assert len(response.results) == len(items) and response.batch_id == first + offset
            for item, result in zip(items, response.results):
                assert result.status.code == 0 and result.achieved_durability == pb.DURABILITY_DURABLE, result
                identity_key = (story, acquired.writer_id, acquired.incarnation, item.sequence)
                assert (result.id.story_id, result.id.writer_id, result.id.incarnation, result.id.sequence) == identity_key
                expected.append((*identity_key, *hlc_key(result.assigned_hlc), item.envelope.payload))
        self.sequences[(story, identity)] += count
        return story, expected

    def append_all(self, count):
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            futures = [pool.submit(self.append_writer, story, identity, acquired, count)
                       for story in self.stories for identity, acquired in self.writers[story]]
            for future in futures:
                story, expected = future.result(timeout=90)
                self.expected[story].extend(expected)

    def read_all(self):
        for story in self.stories:
            expected = sorted(self.expected[story], key=lambda e: (e[4], e[5], e[1], e[2], e[3]))
            start = min((e[4], e[5]) for e in expected)
            end = max((e[4], e[5]) for e in expected)
            self.replay = self.rpc.ReplayStub(self.connect(self.writers[story][0][1].route.player))
            self.read_complete(self.pb.ReadRequest(story_id=story, hlc=self.pb.HlcRange(
                start=self.pb.Hlc(physical_ns=start[0], logical=start[1]),
                end=self.pb.Hlc(physical_ns=end[0], logical=end[1]+1))), expected)

    def settled(self):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            control('snapshot', '')
            records = []
            for writer in ('grapher-a', 'grapher-b'):
                path = ARCHIVE / 'manifest' / (writer + '.log')
                assert path.exists(), path
                for line in path.read_text().splitlines():
                    record = json.loads(line)
                    if 'chunk' not in record:
                        continue
                    assert record['writer'] == writer, record
                    if record['story'] in self.stories:
                        assert writer == ['grapher-a', 'grapher-b'][record['story'] % 2]
                        records.append(record)
            chunk_ids = {r['chunk'] for r in records}
            settled = set()
            for keeper in (r['keeper'] for r in TABLE):
                settled.update(re.findall(r'archive_settled chunk=(\S+) story=(\d+)', (OUT / (keeper+'.log')).read_text()))
            settled_ids = {chunk for chunk, story in settled if int(story) in self.stories}
            visible = set(re.findall(r'archive_visible chunk=(\S+)', (OUT / 'manifest-probe.log').read_text()))
            counts = {story: sum(r['count'] for r in records if r['story'] == story) for story in self.stories}
            if (chunk_ids and chunk_ids == settled_ids and chunk_ids <= visible
                    and all(counts[story] == len(self.expected[story]) for story in self.stories)):
                time.sleep(2)
                return len(chunk_ids), len(settled_ids)
            time.sleep(.3)
        raise RuntimeError(f'settlement: published={len(chunk_ids)} settled={len(settled_ids)} visible={len(visible)} counts={counts}')

    def measure(self):
        control('snapshot', '')
        published = {}
        for chunk, timestamp in re.findall(r'archive_published chunk=(\S+) story=\d+ monotonic_ns=(\d+)', (OUT/'grapher-b.log').read_text()):
            published.setdefault(chunk, int(timestamp))
        samples = []
        for chunk, writer, timestamp in re.findall(r'archive_visible chunk=(\S+) story=\d+ writer=(\S+) count=\d+ monotonic_ns=(\d+)', (OUT/'manifest-probe.log').read_text()):
            if writer == 'grapher-b' and chunk in published:
                samples.append(max(0, int(timestamp) - published[chunk]) / 1e6)
        assert samples, 'no NFS latency samples'
        result = dict(samples=len(samples), median_ms=statistics.median(samples), max_ms=max(samples),
                      build='dev Debug', commit=os.environ.get('CHRONOLOG_CLUSTER_COMMIT', 'unknown'),
                      publisher='blade grapher-b', reader='blade Player FileTierStore merged manifest probe',
                      poll_ms=200, archive=str(ARCHIVE))
        (OUT/'latency.json').write_text(json.dumps(result, indent=2)+'\n')
        print('PASS latency ' + json.dumps(result), flush=True)

    def run_cluster(self):
        self.a7()
        self.setup()
        self.append_all(1000)
        self.settled()
        self.read_all()
        print('PASS b ten stories both Graphers three Keepers per story 3000 DURABLE events complete', flush=True)
        published, settled = self.settled()
        print(f'PASS c merged Player manifest published={published} settled={settled} every event counted', flush=True)
        control('pause', 'grapher-a')
        previous = control('transfer-log', '')
        self.append_all(200)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            current = control('transfer-log', '')
            attempts = re.findall(r'archive_transfer_start chunk=(\S+) grapher=100.101.232.95:50053', current)
            old = set(re.findall(r'archive_transfer_start chunk=(\S+) grapher=100.101.232.95:50053', previous))
            if any(chunk not in old for chunk in attempts):
                break
            time.sleep(.1)
        else:
            raise RuntimeError('no in-flight transfer to paused grapher-a')
        control('kill', 'grapher-a')
        control('start', 'grapher-a')
        self.append_all(20)
        self.settled()
        self.read_all()
        print('PASS d grapher-a SIGKILL with an in-flight transfer restart append complete no duplicates', flush=True)
        control('pause', 'grapher-a')
        control('pause', 'grapher-b')
        self.append_all(20)
        control('kill', 'keeper-1')
        control('start', 'keeper-1')
        control('resume', 'grapher-a')
        control('resume', 'grapher-b')
        for story in self.stories:
            for index, (identity, previous) in enumerate(self.writers[story]):
                if previous.assigned_keeper.process_id == 'keeper-1':
                    # Keeper SIGKILL recovery is an explicit takeover of the writer's own previous incarnation.
                    acquired = self.acquire(story, identity, prior=previous.incarnation, lease_ns=self.MAX_LEASE_NS)
                    assert acquired.status.code == 0 and acquired.incarnation > previous.incarnation
                    self.writers[story][index] = (identity, acquired)
                    self.sequences[(story, identity)] = 0
        self.append_all(20)
        self.settled()
        self.read_all()
        print('PASS e keeper-1 SIGKILL unarchived DURABLE events survive WAL restart resumed writers read complete', flush=True)
        self.measure()
        self.run(30)
        print('PASS A7d G6 scenario cases and inherited smoke assertions', flush=True)


def main():
    stubs = OUT / 'stubs'
    scenario = Scenario(stubs)
    try:
        scenario.run_cluster()
    finally:
        for channel in scenario.channels:
            channel.close()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'FAIL scenario {error}', flush=True)
        raise
