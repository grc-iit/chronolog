import json
from pathlib import Path
import re
import sys
import tempfile
import types

HERE = Path(__file__).resolve().parent
CLUSTER = HERE.parent
ROOT = CLUSTER.parents[1]
sys.path.insert(0, str(CLUSTER))
import cluster
from topology import Lab


def example_lab_resolves_hosts_from_the_file():
    lab = Lab(CLUSTER / 'lab.example.json')
    assert lab.locks == [] and lab.driver_env == {}
    assert lab.workdir_shell() == '"$HOME"/chronolog-lab'
    command = cluster.Cluster.command(types.SimpleNamespace(lab=lab), 'node1', 'true')
    assert command[0] == 'systemd-run' and 'ssh' not in command
    command = cluster.Cluster.command(types.SimpleNamespace(lab=lab), 'node3', 'true')
    assert command[:6] == ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', 'lab@10.0.0.13']
    assert cluster.Cluster.command(types.SimpleNamespace(lab=lab), 'node2', 'true')[5] == 'node2'
    assert lab.hardware('node2').endswith('findmnt /mnt/shared || true')
    dynamic = lab.data['dynamic']
    for placed in dynamic['blocks'] + [dynamic['grapher'], dynamic['player']]:
        assert placed['host'] in lab.nodes, placed


def grapher_order_follows_the_file():
    data = json.loads((CLUSTER / 'lab.example.json').read_text())
    data['graphers'] = ['node3', 'node1']
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / 'lab.json'
        path.write_text(json.dumps(data))
        lab = Lab(path)
    assert lab.grapher_endpoints() == ['10.0.0.13:50053', '10.0.0.11:50053']
    assert lab.row('node3')['grapher'] == 'grapher-a' and lab.row('node1')['grapher'] == 'grapher-b'
    assert lab.plan()['probe'] == 'player-1'
    assert lab.configs({node: '/w' for node in lab.nodes}, 't')['visor-2'][1]['graphers'] == lab.grapher_endpoints()


def invalid_labs_are_refused():
    base = json.loads((CLUSTER / 'lab.example.json').read_text())
    broken = [dict(base, hosts=base['hosts'][:2]), dict(base, driver='node9'), dict(base, graphers=['node1']),
              dict(base, graphers=['node1', 'node9']), dict(base, ports={k: v for k, v in base['ports'].items()
                                                                       if k != 'raft'}),
              dict(base, locks=[dict(host='node9', command='true')])]
    with tempfile.TemporaryDirectory() as folder:
        for index, data in enumerate(broken):
            path = Path(folder) / f'{index}.json'
            path.write_text(json.dumps(data))
            try:
                Lab(path)
            except ValueError:
                continue
            raise AssertionError(f'accepted {data}')


def code_names_no_lab_host():
    sources = [CLUSTER / name for name in ('cluster.py', 'scenario.py', 'topology.py', 'agent.py')]
    sources.append(ROOT / 'tests/integration/dynamic/run.py')
    for config in ('lab.json', 'lab.example.json'):
        if not (CLUSTER / config).exists():
            continue
        lab = Lab(CLUSTER / config)
        for word in [*lab.nodes, *lab.nodes.values(), lab.archive, lab.workdir.lstrip('~/')]:
            pattern = re.compile(r'(?<![\w.-])' + re.escape(word) + r'(?![\w-])')
            for source in sources:
                found = pattern.search(source.read_text())
                assert not found, f'{source} names {word!r} from {config}'


if __name__ == '__main__':
    globals()[sys.argv[1]]()
    print('PASS ' + sys.argv[1])
