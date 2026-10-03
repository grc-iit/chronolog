#!/usr/bin/env python3
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
OUT = ROOT / 'build/cluster'
ARCHIVE = Path('/mnt/nfs/chronolog-sprint/archive')
sys.path.insert(0, str(ROOT / 'tests/smoke/python'))
from smoke import Smoke, hlc_key


def control(op, service):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(90)
        client.connect(os.environ['CHRONOLOG_CLUSTER_SOCKET'])
        client.sendall(json.dumps([op, service]).encode())
        reply = json.loads(client.recv(8192))
    if 'error' in reply:
        raise RuntimeError(reply['error'])
    return reply['output']


class Scenario(Smoke):
    # The Visor's configured acquisition_lease_max_ns. This raw driver never renews: it takes the maximum lease
    # and append_writer asserts every hold stays inside the returned grant.
    MAX_LEASE_NS = 3_600_000_000_000

    def __init__(self, stubs):
        super().__init__(stubs, '100.124.181.9:50051', 10, 'docker', 'cluster', [])
        self.stories = []
        self.writers = {}
        self.expected = {}
        self.sequences = {}

    def setup(self):
        pb = self.pb
        chronicle = 'cluster-' + uuid.uuid4().hex[:8]
        response = self.catalog.CreateChronicle(pb.CreateChronicleRequest(name=chronicle), timeout=10)
        assert response.status.code == 0, response.status
        for i in range(10):
            response = self.catalog.CreateStory(pb.CreateStoryRequest(chronicle=chronicle, name=f's{i}'), timeout=10)
            assert response.status.code == 0, response.status
            story = response.story.story_id
            self.stories.append(story)
            self.expected[story] = []
            self.writers[story] = []
            for writer in range(2):
                identity = f'{chronicle}-{i}-{writer}'
                acquired = self.acquire(story, identity, lease_ns=self.MAX_LEASE_NS)
                assert acquired.status.code == 0, acquired.status
                assert acquired.lease.duration_ns == self.MAX_LEASE_NS, acquired.lease
                assert acquired.route.grapher == ['100.101.232.95:50053', '100.124.181.9:50053'][story % 2]
                self.writers[story].append((identity, acquired))
                self.sequences[(story, identity)] = 0
            assert len({a.assigned_keeper.process_id for _, a in self.writers[story]}) == 2
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
                durability=pb.DURABILITY_DURABLE, batch_id=first+offset, items=items), timeout=10)
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
            for keeper in ('keeper-1', 'keeper-2'):
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
        self.setup()
        self.append_all(1000)
        self.settled()
        self.read_all()
        print('PASS b ten stories both Graphers two Keepers per story 2000 DURABLE events complete', flush=True)
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


def main():
    stubs = OUT / 'stubs'
    subprocess.run([str(ROOT/'tests/smoke/python/gen_stubs.sh'), str(stubs)], timeout=30, check=True,
                   env={**os.environ, 'PYTHON': sys.executable})
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
