#!/usr/bin/env python3
"""Count one Tail's committed Player registrations using the existing dynamic harness."""
import argparse
import importlib.util
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / 'tests/integration/dynamic'
sys.path.insert(0, str(HARNESS))
spec = importlib.util.spec_from_file_location('dynamic_run', HARNESS / 'run.py')
harness = importlib.util.module_from_spec(spec)
spec.loader.exec_module(harness)
from scenario import Scenario


def fields(data):
    offset = 0
    def varint():
        nonlocal offset
        value = shift = 0
        while True:
            byte = data[offset]
            offset += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7
    result = {}
    while offset < len(data):
        tag = varint()
        kind = tag & 7
        if kind == 0:
            value = varint()
        elif kind == 2:
            size = varint()
            value = data[offset:offset + size]
            offset += size
        elif kind in (1, 5):
            size = 8 if kind == 1 else 4
            value = data[offset:offset + size]
            offset += size
        else:
            raise ValueError('unsupported protobuf wire type')
        result[tag >> 3] = value
    return result


def player_registration(blob):
    # NuRaft log_entry::serialize stores an eight-byte term and one-byte type before the command.
    command = fields(blob[9:])
    membership = fields(command.get(8, b''))
    register = fields(membership.get(1, b''))
    process = fields(register.get(1, b''))
    return process.get(1) == b'player-1'


def sample(catalog, raft, previous):
    committed = catalog.execute("SELECT value FROM counters WHERE name='raft_index'").fetchone()[0]
    rows = raft.execute('SELECT idx,value FROM logs WHERE idx>? AND idx<=? ORDER BY idx',
                        (previous, committed)).fetchall()
    if committed > previous:
        assert rows and rows[0][0] == previous + 1 and rows[-1][0] == committed, 'measurement lost compacted entries'
    return committed, sum(player_registration(blob) for _, blob in rows)


def measure(stack, scenario, probe, label):
    databases = [sqlite3.connect(f'file:{stack.folder}/visor-{n}.sqlite?mode=ro', uri=True) for n in (1, 2, 3)]
    # No election is induced; replica 3 has the harness's shortest election timer.
    catalog = databases[2]
    raft = sqlite3.connect(f'file:{stack.folder}/visor-3.sqlite.raft?mode=ro', uri=True)
    start_index = catalog.execute("SELECT value FROM counters WHERE name='raft_index'").fetchone()[0]
    previous, registrations = start_index, 0
    started = time.monotonic()
    client = subprocess.Popen(['timeout', '15', probe, scenario.player, str(scenario.story), '10'],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        while client.poll() is None and time.monotonic() - started < 15:
            previous, count = sample(catalog, raft, previous)
            registrations += count
            time.sleep(.05)
        output, _ = client.communicate(timeout=2)
        assert client.returncode == 0, output
        previous, count = sample(catalog, raft, previous)
        registrations += count
        elapsed = time.monotonic() - started
        result = dict(label=label, seconds=elapsed, player_register_calls=registrations,
                      player_register_per_second=registrations / elapsed,
                      visor_raft_commits=previous - start_index,
                      visor_raft_commits_per_second=(previous - start_index) / elapsed,
                      committed_start=start_index, committed_end=previous, tail=output.strip())
        print(json.dumps(result), flush=True)
    finally:
        if client.poll() is None:
            client.kill()
            client.wait(timeout=2)
        raft.close()
        for database in databases:
            database.close()


def main():
    parser = argparse.ArgumentParser()
    for name in ('rpc', 'visor', 'keeper', 'grapher', 'before', 'after', 'probe'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    args.player = args.before
    stack = scenario = None
    try:
        for attempt in range(harness.STARTUP_ATTEMPTS):
            try:
                stack = harness.Local(args)
                peers = harness.configure(stack)
                node, _, config = stack.services['player']
                config['tail_poll_ms'] = 10
                stack.write('player', node, config)
                for role in stack.services:
                    stack.start(role)
                stack.alive()
                break
            except harness.StartupError:
                stack.close()
                if attempt + 1 == harness.STARTUP_ATTEMPTS:
                    raise
        scenario = Scenario(stack, peers, args.rpc)
        scenario.call('CreateChronicle', dict(name='route-measure'))
        created = scenario.call('CreateStory', dict(chronicle='route-measure', name='tail'))
        scenario.story = int(created['story']['story_id'])
        scenario.wait(lambda: all(any(i.get('granted') for i in m.get('instances', []))
            for m in scenario.state()['members'] if m['process']['process_id'].startswith('keeper-')))
        def player_instance():
            return next((m['process']['instance'] for m in scenario.state()['members']
                         if m['process']['process_id'] == 'player-1'), None)
        previous_instance = scenario.wait(player_instance)
        measure(stack, scenario, args.probe, 'before')
        stack.stop('player')
        args.player = args.after
        stack.start('player')
        scenario.wait(lambda: player_instance() not in (None, previous_instance))
        measure(stack, scenario, args.probe, 'after')
    finally:
        if scenario:
            scenario.close()
        if stack:
            stack.close()
            print('Logs ' + str(stack.folder), flush=True)


if __name__ == '__main__':
    main()
