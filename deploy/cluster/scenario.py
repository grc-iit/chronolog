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
import chronolog as cl
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


class SdkCatalog:
    def __init__(self, scenario, raw):
        self.scenario, self.raw = scenario, raw

    def __getattr__(self, name):
        return getattr(self.raw, name)

    def Release(self, request, timeout=None):
        writer = self.scenario.sdk_writers[(request.story_id, request.writer_id, request.incarnation)]
        return self.scenario.pb.ReleaseResponse(fenced=writer.release(timeout=timeout))


class SdkJournal:
    def __init__(self, scenario):
        self.scenario = scenario

    def Append(self, request, timeout=None):
        s, pb = self.scenario, self.scenario.pb
        assert request.items, request
        first = request.items[0]
        key = (request.story_id, first.writer_id, first.incarnation)
        writer = s.sdk_writers[key]
        assert all((item.writer_id, item.incarnation) == key[1:] for item in request.items)
        assert [item.sequence for item in request.items] == list(range(
            s.sdk_sequences[key] + 1, s.sdk_sequences[key] + len(request.items) + 1))
        specs = [cl.AppendSpec(cl.Envelope(item.envelope.payload, item.envelope.content_type,
            dict(item.envelope.attributes), item.envelope.trace_id, item.envelope.span_id),
            cl.Durability(request.durability), cl.TimeReading(item.physical.physical_ns,
                item.physical.uncertainty_ns if item.physical.HasField('uncertainty_ns') else None,
                {pb.CLOCK_STATUS_SYNCED: 0, pb.CLOCK_STATUS_UNSYNCED: 1,
                 pb.CLOCK_STATUS_UNAVAILABLE: 2, pb.CLOCK_STATUS_UNSPECIFIED: 2}[
                    item.physical.status])) for item in request.items]
        if len(specs) == 1:
            try:
                results = [writer.append(specs[0].envelope.payload,
                    content_type=specs[0].envelope.content_type, attributes=specs[0].envelope.attributes,
                    trace_id=specs[0].envelope.trace_id, span_id=specs[0].envelope.span_id,
                    durability=specs[0].durability, physical=specs[0].physical, timeout=timeout)]
            except cl.Error as error:
                results = [error]
        else:
            results = writer.append_batch(specs, timeout=timeout)
        response = pb.AppendResponse(batch_id=request.batch_id)
        for item, result in zip(request.items, results):
            if isinstance(result, cl.Error):
                response.results.add(status=pb.ItemStatus(code=int(result.status.code), message=result.status.message),
                                     rejection=int(result.status.rejection))
                continue
            event = result.event_id
            assert (event.story_id, event.writer_id, event.incarnation, event.sequence) == (*key, item.sequence)
            if item.HasField('causal_floor'):
                assert hlc_key(result.hlc) > hlc_key(item.causal_floor)
            response.results.add(id=pb.EventId(story_id=event.story_id, writer_id=event.writer_id,
                incarnation=event.incarnation, sequence=event.sequence),
                assigned_hlc=pb.Hlc(physical_ns=result.hlc.physical_ns, logical=result.hlc.logical),
                achieved_durability=int(result.durability))
        assert len(response.results) == len(request.items)
        if all(result.status.code == 0 for result in response.results):
            s.sdk_sequences[key] += len(request.items)
        return response


class Scenario(Smoke):
    # The SDK owns renewal, Route refresh and typed append retries within each call deadline.
    MAX_LEASE_NS = 3_600_000_000_000

    def __init__(self, stubs, catalogs=None, visor=None):
        cl.__path__.append(str(Path(stubs) / 'chronolog'))
        super().__init__(stubs, visor or TABLE[0]['ip'] + ':50051', 10, 'docker', 'cluster', [])
        self.sdk = cl.connect(catalogs or ','.join(r['ip'] + ':50051' for r in TABLE),
                              timeout=10, max_retries=500, retry_backoff=.02)
        self.sdk_writers = {}
        self.sdk_sequences = {}
        self.catalog = SdkCatalog(self, self.catalog)
        raw_rpc = self.rpc
        class Rpc:
            CatalogStub = staticmethod(lambda channel: SdkCatalog(self, raw_rpc.CatalogStub(channel)))
            ReplayStub = raw_rpc.ReplayStub
            JournalStub = staticmethod(lambda channel: SdkJournal(self))
        self.rpc = Rpc
        self.stories = []
        self.writers = {}
        self.expected = {}
        self.sequences = {}

    def settled_chunks(self, story):
        logs = control('transfer-log', '')
        pattern = rf'archive_settled chunk=\S+ story={story} start=(\d+):(\d+) end=(\d+):(\d+)'
        return sorted(((int(m[1]), int(m[2])), (int(m[3]), int(m[4]))) for m in re.finditer(pattern, logs))

    def hold(self, acquired):
        lease = self.sdk_writers[(acquired.story_id, acquired.writer_id, acquired.incarnation)].lease()
        assert lease.grant.duration_ns > 0, lease

    def append(self, acquired, sequence, durability, floor=None):
        item = self.pb.AppendItem(writer_id=acquired.writer_id, incarnation=acquired.incarnation, sequence=sequence,
            physical=self.pb.TimeReading(physical_ns=time.time_ns(), status=self.pb.CLOCK_STATUS_UNSYNCED),
            envelope=self.pb.Envelope(payload=f'event-{sequence}'.encode()))
        if floor is not None:
            item.causal_floor.CopyFrom(floor)
        return SdkJournal(self).Append(self.pb.AppendRequest(story_id=acquired.story_id,
            epoch=acquired.route.epoch, items=[item], durability=durability, batch_id=sequence),
            timeout=self.timeout).results[0]

    def close(self):
        for writer in self.sdk_writers.values():
            try:
                writer.release(timeout=5)
            except cl.Error:
                pass
        for channel in self.channels:
            channel.close()

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
        try:
            writer = self.sdk.acquire(story, identity, options=cl.AcquireOptions(
                lease_duration_ns=lease_ns or self.MAX_LEASE_NS, preferred_keeper_process_id=preferred,
                takeover=prior is not None, expected_prior_incarnation=prior), timeout=30)
        except cl.Error as error:
            return self.pb.AcquireResponse(status=self.pb.ItemStatus(code=int(error.status.code),
                                                                     message=error.status.message))
        acquired = writer.acquisition
        key = (acquired.story_id, acquired.writer_id, acquired.incarnation)
        self.sdk_writers[key] = writer
        self.sdk_sequences[key] = 0
        route = self.pb.Route(epoch=acquired.route.epoch, player=acquired.route.player,
                             grapher=acquired.route.grapher, keepers=[self.pb.KeeperRef(
                process_id=k.process_id, endpoint=k.endpoint) for k in acquired.route.keepers])
        return self.pb.AcquireResponse(story_id=acquired.story_id, writer_id=acquired.writer_id,
            incarnation=acquired.incarnation, route=route,
            keeper_preference=int(acquired.keeper_preference or 0), assigned_keeper=self.pb.KeeperRef(
                process_id=acquired.assigned_keeper.process_id, endpoint=acquired.assigned_keeper.endpoint),
            lease=self.pb.AcquisitionLease(duration_ns=acquired.lease.duration_ns,
                                            remaining_ns=acquired.lease.remaining_ns))

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
            response = journal.Append(pb.AppendRequest(story_id=story, epoch=acquired.route.epoch,
                durability=pb.DURABILITY_DURABLE, batch_id=first+offset, items=items),
                timeout=self.timeout)
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
        scenario.close()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(f'FAIL scenario {error}', flush=True)
        raise
