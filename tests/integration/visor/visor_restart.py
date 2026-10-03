#!/usr/bin/env python3
"""Visor crash and failover gate (section 12, I9.1), on the dynamic harness of tests/integration/dynamic.

Three Visor replicas, two Keepers, a Grapher and a Player on loopback. A writer appends through the Keepers while
the Visor leader is killed, then while all three Visors are killed and restarted. The Catalog revision counter
never goes backwards, a writer's acquisition survives with the same writer_id and a higher incarnation, writers keep
appending, and every acknowledged DURABLE event is in a complete read (RevisionSurvivesLeaderChange at process level).
The harness classes are reused by import; nothing in tests/integration/dynamic is modified.
"""
import argparse
import signal
import sys
import threading
import time
from pathlib import Path

DYNAMIC = Path(__file__).resolve().parents[1] / 'dynamic'
sys.path.insert(0, str(DYNAMIC))
import run  # noqa: E402
from scenario import Scenario  # noqa: E402


class Direct(run.Local):
    """The dynamic stack with Keepers connecting to the Visors directly. The harness proxy only exists to partition a
    Keeper, which this gate never does, and the product reaches the replicas through one gRPC channel over the list."""

    def configure_direct(self):
        for role in [r for r in self.services if r.startswith('proxy-')]:
            del self.services[role]

    def start(self, role):
        node, path, config = self.services[role]
        if not role.startswith('keeper-'):
            return super().start(role)
        log = open(self.folder / (role + '.log'), 'ab')
        process = run.subprocess.Popen(['timeout', '-k', '5', '340', self.args.keeper, '--config', str(path)],
                                       stdout=log, stderr=log, start_new_session=True)
        self.processes[role] = (process, log)
        self.ready(role, run.endpoints(config), process)


class Workload(threading.Thread):
    """Appends one event after another through its own RPC probe until stopped."""

    def __init__(self, stack, peers, binary, story, acquired):
        super().__init__(daemon=True)
        self.probe = Scenario(stack, peers, binary)
        self.probe.story = story
        self.probe.expected[story] = {}
        self.acquired = acquired
        self.stop_flag = threading.Event()
        self.error = None
        self.sequence = 0

    def run(self):
        try:
            while not self.stop_flag.is_set():
                self.sequence += 1
                # A refused append is retried, so the sequence always advances through acknowledged events.
                self.probe.append(self.acquired, self.sequence)
                self.stop_flag.wait(0.02)
        except Exception as error:  # noqa: BLE001
            self.error = error

    def finish(self):
        self.stop_flag.set()
        self.join(timeout=60)
        if self.is_alive():
            raise RuntimeError('workload did not stop')
        self.probe.close()
        if self.error:
            raise RuntimeError(f'workload failed: {self.error}')
        return self.probe.expected


class Gate(Scenario):
    def revision(self):
        # Acquire and release a private writer: the release reports the persisted global Catalog counter.
        a = self.acquire('revision-probe')
        released = self.rpc('Release', dict(story_id=self.story, writer_id=a['writer_id'],
                                            incarnation=a['incarnation']))
        return int(released['revision'])

    def acked(self, workload):
        return workload.sequence

    def progress(self, workload, beyond, seconds=60):
        # The workload keeps appending: its acknowledged sequence advances past `beyond`.
        self.wait(lambda: workload.sequence > beyond, seconds)

    def run(self):
        self.wait(lambda: self.rpc('CreateChronicle', dict(name='visor-restart')))
        self.story = int(self.rpc('CreateStory', dict(chronicle='visor-restart', name='story'))['story']['story_id'])
        self.expected[self.story] = {}
        self.wait(lambda: len([m for m in self.state()['members']
                               if m['process']['process_id'].startswith('keeper-')
                               and any(i.get('granted') for i in m.get('instances', []))]) == 2)
        main = self.acquire('main-writer')
        background = self.acquire('background-writer')
        for sequence in range(1, 9):
            self.append(main, sequence)
        self.complete()
        workload = Workload(self.stack, self.peers, self.probe_binary, self.story, background)
        workload.start()
        try:
            self.progress(workload, 5)

            started = self.begin('leader kill mid-workload')
            leader = self.leader()
            revision_before = self.revision()
            sequence_before = workload.sequence
            self.stack.stop('visor-' + str(leader))
            follower = self.peers[leader % 3]
            self.endpoint, self.internal = follower['catalog_endpoint'], follower['internal_endpoint']
            replacement = self.wait(lambda: self.acquire('main-writer'))
            assert replacement['writer_id'] == main['writer_id'], 'I9.1 writer identity lost across the leader change'
            assert int(replacement['incarnation']) > int(main['incarnation']), 'I9.1 incarnation reused after leader kill'
            revision_after = self.wait(self.revision)
            assert revision_after > revision_before, f'revision went back: {revision_before} -> {revision_after}'
            self.progress(workload, sequence_before + 5)
            self.append(replacement, 1)
            self.mark('leader kill mid-workload I9.1', started)

            started = self.begin('leader restart')
            self.stack.start('visor-' + str(leader))
            self.progress(workload, workload.sequence + 5)
            self.mark('leader restart I9.1', started)

            started = self.begin('all Visors killed and restarted')
            revision_before = self.wait(self.revision)
            main = self.wait(lambda: self.acquire('main-writer'))
            for n in (1, 2, 3):
                self.stack.stop('visor-' + str(n))
            sequence_before = workload.sequence
            for n in (3, 1, 2):
                self.stack.start('visor-' + str(n))
            self.endpoint, self.internal = self.peers[2]['catalog_endpoint'], self.peers[2]['internal_endpoint']
            restarted = self.wait(lambda: self.acquire('main-writer'), 60)
            assert restarted['writer_id'] == main['writer_id'], 'I9.1 writer identity lost across restart'
            assert int(restarted['incarnation']) > int(main['incarnation']), 'I9.1 incarnation reused after restart'
            revision_after = self.wait(self.revision, 60)
            assert revision_after > revision_before, f'revision went back: {revision_before} -> {revision_after}'
            self.progress(workload, sequence_before + 5, 90)
            self.append(restarted, 1)
            self.mark('all Visors restarted I9.1', started)
        finally:
            events = workload.finish()
        for key, event in events[self.story].items():
            self.expected[self.story][key] = event
        self.complete()
        print('PASS Catalog revision and acquisitions survived leader kill and full restart, writers kept appending',
              flush=True)

    def leader(self):
        seen = []

        def led():
            reply = self.raw('Acquire', dict(story_id=self.story, writer_identity='leader-probe'))
            assert reply['transport'] == 0 or self.refused_before_apply(reply), f'leader probe Acquire: {reply}'
            if reply['transport'] == 0 and reply.get('leader'):
                seen[:] = [int(reply['leader'])]
            return seen[0] if seen else None
        return self.wait(led, 15)


def main():
    parser = argparse.ArgumentParser()
    for role in ('rpc', 'visor', 'keeper', 'grapher', 'player'):
        parser.add_argument('--' + role, required=True)
    args = parser.parse_args()

    def interrupted(signum, frame):
        raise RuntimeError('driver interrupted by signal ' + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    stack = None
    try:
        for attempt in range(1, run.STARTUP_ATTEMPTS + 1):
            try:
                stack = Direct(args)
                peers = run.configure(stack)
                stack.configure_direct()
                for role in stack.services:
                    stack.start(role)
                stack.alive()
                break
            except run.StartupError as error:
                print(f'STARTUP attempt {attempt}/{run.STARTUP_ATTEMPTS} failed: {error}', flush=True)
                if stack:
                    stack.close()
                    stack = None
                if attempt == run.STARTUP_ATTEMPTS:
                    print('FAIL startup: ' + str(error), flush=True)
                    return 1
        gate = Gate(stack, peers, args.rpc)
        gate.probe_binary = args.rpc
        try:
            gate.run()
        except Exception as error:  # noqa: BLE001
            print(f'FAIL {gate.phase} {time.monotonic() - gate.phase_started:.3f}s {error}', flush=True)
            return 1
        finally:
            gate.close()
        return 0
    except Exception as error:  # noqa: BLE001
        print('FAIL driver: ' + str(error), flush=True)
        return 1
    finally:
        if stack:
            stack.close()
            print('Logs ' + str(stack.folder), flush=True)


if __name__ == '__main__':
    sys.exit(main())
