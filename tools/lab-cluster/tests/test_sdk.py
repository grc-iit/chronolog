import importlib
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

import chronolog as cl

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/lab-cluster'))
from agent import Agent, main as agent_request

VISOR = os.environ['CHRONOLOG_TEST_VISOR']
PLAYER = os.environ['CHRONOLOG_TEST_PLAYER']


def test_scenario_sdk_append_projection_and_fencing(tmp_path):
    stubs = tmp_path / 'stubs'
    stubs.mkdir()
    subprocess.run([sys.executable, '-m', 'grpc_tools.protoc', '-I' + str(ROOT / 'proto'),
        '--python_out=' + str(stubs), '--grpc_python_out=' + str(stubs),
        'chronolog/v1/chronolog.proto'], check=True, timeout=30)
    os.environ['CHRONOLOG_CLUSTER_OUT'] = str(tmp_path)
    scenario_module = importlib.import_module('scenario')
    scenario = scenario_module.Scenario(stubs, catalogs=VISOR, visor=VISOR)
    try:
        pb = scenario.pb
        chronicle = 'sdk-' + uuid.uuid4().hex
        story = scenario.sdk.create_story(scenario.sdk.create_chronicle(chronicle), 'events').id
        acquired = scenario.acquire(story, 'driver', preferred='keeper-1')
        assert acquired.status.code == 0
        assert acquired.assigned_keeper.process_id == 'keeper-1'
        assert acquired.keeper_preference == pb.KEEPER_PREFERENCE_RESULT_HONORED
        scenario.sequences[(story, 'driver')] = 0
        _, first = scenario.append_writer(story, 'driver', acquired, 205)
        _, second = scenario.append_writer(story, 'driver', acquired, 3)
        assert [e[3] for e in first + second] == list(range(1, 209))
        scenario.expected[story] = first + second
        scenario.replay = scenario.rpc.ReplayStub(scenario.connect(PLAYER))
        scenario.read_complete(scenario.request(story), first + second)
        held = scenario.sdk_writers[(story, acquired.writer_id, acquired.incarnation)]
        assert held.lease().grant.duration_ns == scenario.MAX_LEASE_NS
        assert scenario.catalog.Release(pb.ReleaseRequest(story_id=story, writer_id=acquired.writer_id,
            incarnation=acquired.incarnation), timeout=5).fenced
        rejected = scenario.append(acquired, 209, pb.DURABILITY_ACCEPTED)
        assert rejected.status.code == 9 and rejected.achieved_durability == pb.DURABILITY_UNSPECIFIED
        successor = scenario.acquire(story, 'driver')
        assert successor.status.code == 0 and successor.incarnation > acquired.incarnation
        receipt = scenario.append(successor, 1, pb.DURABILITY_DURABLE)
        assert receipt.status.code == 0 and receipt.id.sequence == 1
        bounded = scenario.sdk.create_story(chronicle, 'physical').id
        bounded_writer = scenario.acquire(bounded, 'bounded')
        stamp = time.time_ns()
        request = pb.AppendRequest(story_id=bounded, epoch=bounded_writer.route.epoch,
            batch_id=1, durability=pb.DURABILITY_DURABLE, items=[pb.AppendItem(
                writer_id=bounded_writer.writer_id, incarnation=bounded_writer.incarnation, sequence=1,
                physical=pb.TimeReading(physical_ns=stamp, uncertainty_ns=0, status=pb.CLOCK_STATUS_SYNCED),
                envelope=pb.Envelope(payload=b'bounded'))])
        result = scenario.rpc.JournalStub(scenario.channel).Append(request, timeout=5).results[0]
        assert result.status.code == 0
        with scenario.sdk.read(bounded, cl.Hlc(result.assigned_hlc.physical_ns, result.assigned_hlc.logical),
            cl.Hlc(result.assigned_hlc.physical_ns, result.assigned_hlc.logical + 1), timeout=5) as reader:
            events = list(reader)
        assert len(events) == 1 and events[0].physical.physical_ns == stamp
        assert events[0].physical.uncertainty_ns == 0 and events[0].physical.status == 0
    finally:
        scenario.close()


def test_agent_preserves_writer_across_requests_and_cas(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    client = cl.connect(VISOR, PLAYER, timeout=5)
    story = client.create_story(client.create_chronicle('agent-' + uuid.uuid4().hex), 'events').id
    agent = Agent()
    request = dict(op='acquire', slot='local', node='offline', catalogs=[VISOR], story=story,
        identity='agent', preferred='keeper-1')
    try:
        acquired = agent.request(request)
        writer = agent.slots['local']['writer']
        assert acquired['assigned'] == 'keeper-1'
        assert acquired['keeper_preference'] == 'KEEPER_PREFERENCE_RESULT_HONORED'
        first = agent.request({**request, 'op': 'append', 'count': 3})
        second = agent.request({**request, 'op': 'append', 'count': 2})
        assert agent.slots['local']['writer'] is writer
        assert [e[3] for e in first['events'] + second['events']] == [1, 2, 3, 4, 5]
        takeover = agent.request({**request, 'op': 'reacquire', 'takeover': True})
        assert takeover['request_id'] != acquired['request_id']
        assert agent.slots['local']['writer'].incarnation > writer.incarnation
        after = agent.request({**request, 'op': 'append', 'count': 1})
        assert after['events'][0][3] == 1
        expected = first['events'] + second['events'] + after['events']
        with client.read(story, cl.Hlc(int(expected[0][4]), expected[0][5]),
            cl.Hlc(int(expected[-1][4]), expected[-1][5] + 1), timeout=5) as reader:
            events = list(reader)
        assert [(e.id.incarnation, e.id.sequence, e.envelope.payload.hex()) for e in events] == [
            (e[2], e[3], e[6]) for e in expected]
    finally:
        agent.close()


def test_agent_socket_preserves_live_slot(tmp_path):
    client = cl.connect(VISOR, PLAYER, timeout=5)
    story = client.create_story(client.create_chronicle('socket-' + uuid.uuid4().hex), 'events').id
    path = str(tmp_path / 'agent.sock')
    with open(tmp_path / 'server.log', 'w+') as log:
        server = subprocess.Popen([sys.executable, str(ROOT / 'tools/lab-cluster/agent.py'), '--serve', path],
            cwd=tmp_path, stdout=log, stderr=log)
        try:
            for _ in range(100):
                assert server.poll() is None
                if Path(path).exists():
                    break
                time.sleep(.01)
            assert Path(path).exists()
            request = dict(op='acquire', node='offline', slot='remote', catalogs=[VISOR], story=story,
                           identity='socket', preferred='keeper-1')
            assert agent_request(request, path)['assigned'] == 'keeper-1'
            a = agent_request({**request, 'op': 'append', 'count': 2}, path)
            b = agent_request({**request, 'op': 'append', 'count': 2}, path)
            assert [e[3] for e in a['events'] + b['events']] == [1, 2, 3, 4]
        finally:
            server.terminate()
            server.wait(timeout=10)
