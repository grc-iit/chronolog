import base64
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import time


def hlc(value):
    return int(value.get('physical_ns', 0)), int(value.get('logical', 0))


def wire(value):
    return dict(physical_ns=value[0], logical=value[1])


def event(value):
    identity = value['id']
    return (*hlc(value['hlc']), int(identity['story_id']), int(identity['writer_id']),
            int(identity['incarnation']), int(identity['sequence']), value.get('envelope', {}).get('payload', ''))


class Scenario:
    def __init__(self, stack, peers, binary):
        self.stack, self.peers = stack, peers
        self.endpoint = peers[2]['catalog_endpoint']
        self.internal = peers[2]['internal_endpoint']
        self.player = stack.services['player'][2]['listen']
        self.probe = subprocess.Popen(['timeout', '-k', '5', '330', binary], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, text=True, start_new_session=True)
        self.expected = {}
        self.story = None
        self.policy = None
        self.secondary = None
        self.phase = 'bootstrap'
        self.phase_started = time.monotonic()

    def close(self):
        self.probe.stdin.close()
        try:
            self.probe.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(self.probe.pid, signal.SIGKILL)
            self.probe.wait(timeout=5)

    def raw(self, op, request=None, endpoint=None, timeout=3000):
        self.probe.stdin.write(json.dumps(dict(op=op, request=request or {}, endpoint=endpoint or self.endpoint,
                                              timeout_ms=timeout)) + '\n')
        self.probe.stdin.flush()
        if not select.select([self.probe.stdout], [], [], timeout / 1000 + 5)[0]:
            raise RuntimeError('RPC probe timeout ' + op)
        line = self.probe.stdout.readline()
        if not line:
            raise RuntimeError('RPC probe exited ' + op)
        result = json.loads(line)
        if 'exception' in result:
            raise RuntimeError(result['exception'])
        return result

    def rpc(self, op, request=None, endpoint=None):
        result = self.raw(op, request, endpoint)
        if result['transport'] or int(result['response'].get('status', {}).get('code', 0)):
            raise RuntimeError(f'{op}: {result}')
        return result['response']

    def call(self, op, request=None, endpoint=None, seconds=30):
        # UNAVAILABLE is retryable: a Visor that lost its lease or has no leader yet refuses before it applies
        # anything. Any other code is the answer.
        last = None
        def attempt():
            nonlocal last
            last = self.raw(op, request, endpoint)
            code = last['transport'] or int(last.get('response', {}).get('status', {}).get('code', 0))
            assert code in (0, 14), f'{op}: {last}'
            if code:
                print(f'RETRY {op} {last.get("error", last)}', flush=True)
            return (last['response'],) if code == 0 else None
        try:
            return self.wait(attempt, seconds)[0]
        except RuntimeError:
            raise RuntimeError(f'{op} stayed UNAVAILABLE for {seconds}s: {last}')

    def wait(self, fn, seconds=30):
        deadline = time.monotonic() + seconds
        detail = None
        while time.monotonic() < deadline:
            try:
                value = fn()
                if value:
                    return value
                detail = value
            except RuntimeError as error:
                detail = str(error)
            time.sleep(.1)
        raise RuntimeError(f'bounded wait failed: {detail}')

    def state(self):
        return self.call('ListMembers', endpoint=self.internal)

    def route(self):
        return next(r for r in self.state().get('routes', []) if int(r['story_id']) == self.story)

    def applied(self, keeper, revision):
        m = next(m for m in self.state()['members'] if m['process']['process_id'] == keeper)
        return int(m.get('applied_route_revision', 0)) >= revision

    def changed(self, previous, keeper, present):
        r = self.route()
        members = [k['process_id'] for k in r['route']['keepers']]
        return r if int(r['route']['epoch']) > previous and ((keeper in members) == present) else None

    def acquire(self, identity):
        return self.rpc('Acquire', dict(story_id=self.story, writer_identity=identity))

    def append(self, acquired, sequence=1, retry=True, whole_rpc_unavailable=False):
        payload = base64.b64encode(f"{self.story}:{acquired['writer_id']}:{acquired['incarnation']}:{sequence}".encode()).decode()
        request = dict(story_id=self.story, epoch=acquired['route']['epoch'], durability='DURABILITY_DURABLE',
                       items=[dict(writer_id=acquired['writer_id'], incarnation=acquired['incarnation'],
                                   sequence=sequence, physical=dict(physical_ns=time.time_ns(), status='CLOCK_STATUS_UNSYNCED'),
                                   envelope=dict(payload=payload))])
        def attempt():
            request['items'][0]['physical']['physical_ns'] = time.time_ns()
            response = self.raw('Append', request, acquired['assigned_keeper']['endpoint'])
            if response['transport']:
                if whole_rpc_unavailable and response['transport'] == 14:
                    return dict(status=dict(code=14, message=response.get('error', '')), transport=14)
                raise RuntimeError(str(response))
            result = response['response']['results'][0]
            code = int(result.get('status', {}).get('code', 0))
            if code == 0:
                assert result['achieved_durability'] == 'DURABILITY_DURABLE', result
                identifier = result['id']
                assert (int(identifier['story_id']), int(identifier['writer_id']), int(identifier['incarnation']),
                        int(identifier['sequence'])) == (self.story, int(acquired['writer_id']),
                                                       int(acquired['incarnation']), sequence), result
                key = (*hlc(result['assigned_hlc']), self.story, int(acquired['writer_id']),
                       int(acquired['incarnation']), sequence, payload)
                old = self.expected.setdefault(self.story, {}).setdefault(key[2:6], key)
                assert old == key, 'I3 idempotent retry changed acknowledged event'
                return result
            if retry and code in (9, 14):
                return None
            return result
        return self.wait(attempt) if retry else attempt()

    def read(self, start=None, end=None):
        expected = sorted(self.expected[self.story].values())
        start = start or expected[0][:2]
        end = end or (expected[-1][0], expected[-1][1] + 1)
        result = self.raw('Read', dict(story_id=self.story, hlc=dict(start=wire(start), end=wire(end))), self.player)
        if result['transport']:
            raise RuntimeError(str(result))
        actual, completion = [], None
        for frame in result['frames']:
            assert completion is None, 'I6.5 frame after Completion'
            if 'batch' in frame:
                actual.extend(event(e) for e in frame['batch'].get('events', []))
            else:
                completion = frame.get('completion')
        assert completion is not None, 'I6.5 missing Completion'
        assert actual == sorted(set(actual)), 'I6 ordered unique events'
        selected = [e for e in expected if start <= e[:2] < end]
        assert all(e in selected for e in actual), 'unexpected event or altered DURABLE identity/HLC/payload'
        if completion.get('complete', False):
            assert actual == selected, 'I12.1 complete read lost acknowledged DURABLE events'
            assert completion.get('reason', 'INCOMPLETE_REASON_UNSPECIFIED') == 'INCOMPLETE_REASON_UNSPECIFIED'
            assert hlc(completion['frontier']) >= end, 'I6.11 complete seal below end'
        return actual, completion

    def complete(self, start=None, end=None):
        def check():
            actual, completion = self.read(start, end)
            return completion if completion.get('complete') else None
        return self.wait(check)

    def begin(self, name):
        self.phase = name
        self.phase_started = time.monotonic()
        print('RUN ' + name, flush=True)
        return self.phase_started

    def mark(self, name, start):
        line = f'PASS {name} {time.monotonic() - start:.3f}s'
        print(line, flush=True)
        with open(self.stack.folder / 'scenarios.log', 'a') as log:
            log.write(line + '\n')

    def admin(self, op, keeper):
        return self.rpc(op, dict(process_id=keeper), self.internal)

    def settled(self, acquired):
        # W10.14 closes admission until the acquisition snapshot restores fences, independently of route application.
        # That refusal and a writer not yet registered say nothing about I4.8; resend the same append within this bound.
        deadline = time.monotonic() + 30
        while True:
            began = time.time_ns()
            result = self.append(acquired, retry=False, whole_rpc_unavailable=True)
            ended = time.time_ns()
            snapshot_pending = (result.get('transport') == 14 and
                                result['status'].get('message') == 'acquisition snapshot is not applied')
            if result.get('transport') and not snapshot_pending:
                raise RuntimeError(str(result))
            if not snapshot_pending and int(result.get('status', {}).get('code', 0)) != 9:
                return began, result, ended
            assert time.monotonic() < deadline, f'acquisition admission never became ready at its Keeper: {result}'
            time.sleep(.05)

    def deferral(self, keeper, until, turn):
        # A Keeper new to the Route admits only once its clock is within acceptance_budget of the Route's
        # physical floor (I4.8), so before `until` an append to it must answer UNAVAILABLE. Both the floor
        # and the Keeper's applied revision are read from the Catalog, so nothing here depends on timing:
        # an attempt that finished inside the window is asserted, one that straddled it is not counted.
        tolerance = 100_000_000
        for n in range(16):
            acquired = self.acquire(f'deferral-{turn}-{n}')
            if acquired['assigned_keeper']['process_id'] == keeper:
                break
        else:
            raise AssertionError('no writer was assigned to ' + keeper)
        start, result, end = self.settled(acquired)
        code = int(result.get('status', {}).get('code', 0))
        print(f'deferral turn {turn} {keeper} window left {(until - start) / 1e9:.3f}s code {code}', flush=True)
        if end + tolerance < until:
            assert code == 14, f'I4.8 {keeper} admitted {(until - end) / 1e9:.3f}s before its clock reached the floor: {result}'
            return 1
        assert code in (0, 14), result
        return 0

    def files(self, story):
        folder = Path(self.stack.archive) / str(story)
        return [p for p in folder.iterdir() if p.is_file()] if folder.is_dir() else []

    def manifest(self):
        return Path(self.stack.archive) / 'manifest' / 'grapher-a.log'

    def tombstones(self):
        found = set()
        for line in self.manifest().read_text(errors='replace').splitlines():
            try:
                entry = json.loads(line)
            except ValueError:
                continue
            if entry.get('tombstoned'):
                found.add(int(entry['story']))
        return found

    def log_size(self, keeper):
        return (self.stack.folder / (keeper + '.log')).stat().st_size

    def log_since(self, keeper, offset=0):
        return (self.stack.folder / (keeper + '.log')).read_bytes()[offset:].decode(errors='replace')

    def hot(self, keeper, story):
        window = dict(start=dict(physical_ns=0, logical=0), end=dict(physical_ns=2 ** 63 - 1, logical=0))
        return self.raw('FetchHot', dict(story_id=story, hlc=window), self.stack.services[keeper][2]['internal_listen'])

    def serving(self, keeper, story):
        result = self.hot(keeper, story)
        return result['transport'] == 0 and result['trailer']

    def retained(self, keeper, story):
        result = self.hot(keeper, story)
        if result['transport'] == 0 and result['trailer'] and result['events'] > 0:
            return True
        raise RuntimeError(f'{keeper} has no retained events for story {story}: {result}')

    def freed(self, keeper, story):
        result = self.hot(keeper, story)
        if result['transport'] == 9 and 'destroyed' in result['error']:
            assert not result['events'] and not result['trailer'], result
            return True
        raise RuntimeError(f'{keeper} still serves story {story}: {result}')

    def refused_read(self, story):
        end = (2 ** 62, 0)
        result = self.raw('Read', dict(story_id=story, hlc=dict(start=wire((0, 0)), end=wire(end))), self.player)
        if result['transport'] == 9 and not result['frames']:
            return True
        raise RuntimeError(f'Read on destroyed story {story}: {result}')

    def seed(self, chronicle, names):
        # Stories with one writer on each Keeper, so every Keeper holds data for each.
        self.call('CreateChronicle', dict(name=chronicle))
        stories = {}
        for name in names:
            story = int(self.call('CreateStory', dict(chronicle=chronicle, name=name))['story']['story_id'])
            writers = {}
            for n in range(16):
                acquired = self.call('Acquire', dict(story_id=story, writer_identity=f'{name}-{n}'))
                writers.setdefault(acquired['assigned_keeper']['process_id'], [acquired, 0])
                if len(writers) == 2:
                    break
            assert len(writers) == 2, f'no writer was assigned to each Keeper for story {name}'
            self.expected[story] = {}
            stories[name] = dict(id=story, writers=writers)
        return stories

    def feed(self, record, count):
        self.story = record['id']
        for slot in record['writers'].values():
            for _ in range(count):
                slot[1] += 1
                self.append(slot[0], slot[1])

    def release(self, record):
        for acquired, _ in record['writers'].values():
            self.call('Release', dict(story_id=record['id'], writer_id=acquired['writer_id'],
                                      incarnation=acquired['incarnation']))

    def archived(self, record):
        return bool(self.files(record['id'])) and all(
            f'archive_settled chunk={keeper}:{record["id"]}:' in self.log_since(keeper) for keeper in record['writers'])

    def destroyed(self, record):
        story = record['id']
        assert self.call('GetStory', dict(story_id=story))['story'].get('tombstoned'), f'story {story} is not tombstoned'
        for keeper in ('keeper-1', 'keeper-2'):
            self.wait(lambda: self.freed(keeper, story))
        self.wait(lambda: story in self.tombstones() and not self.files(story))
        self.wait(lambda: self.refused_read(story))

    def member(self, keeper):
        return next(m for m in self.state()['members'] if m['process']['process_id'] == keeper)

    def rejoin(self, keeper, primary):
        # A partition past keeper_failure_timeout_ms drains the Keeper from every Route. It registers again by
        # itself and JoinKeeper puts it back.
        self.wait(lambda: any(i.get('granted') for i in self.member(keeper).get('instances', [])))
        if not self.member(keeper).get('joined'):
            self.admin('JoinKeeper', keeper)
        self.wait(lambda: self.member(keeper).get('joined'))
        self.story = primary
        self.wait(lambda: self.applied(keeper, int(self.route()['revision'])))

    def destroy(self):
        primary = self.story
        keepers = ('keeper-1', 'keeper-2')
        cases = self.seed('destroy', ['gone', 'orphan', 'sibling', 'resume', 'missed'])
        pair = self.seed('destroy-all', ['first', 'second'])
        everything = [*cases.values(), *pair.values()]
        for record in everything:
            self.feed(record, 4)
        self.wait(lambda: all(self.archived(record) for record in everything), 90)

        # Archived chunks at the Grapher and fresh events at the Keepers, then the destroy.
        gone = cases['gone']
        self.feed(gone, 2)
        self.release(gone)
        self.wait(lambda: all(self.retained(keeper, gone['id']) for keeper in keepers))
        self.call('DestroyStory', dict(story_id=gone['id']))
        self.destroyed(gone)
        after_gone = {keeper: self.log_size(keeper) for keeper in keepers}

        # The Grapher is down when the story is destroyed: the Keepers free their retained chunks without it
        # (I13.9) and the Grapher, back later, records the tombstone and deletes the files.
        orphan, sibling = cases['orphan'], cases['sibling']
        self.stack.stop('grapher-a')
        self.feed(orphan, 2)
        self.feed(sibling, 2)
        self.release(orphan)
        self.wait(lambda: all(self.retained(keeper, orphan['id']) for keeper in keepers))
        self.call('DestroyStory', dict(story_id=orphan['id']))
        for keeper in keepers:
            self.wait(lambda: self.freed(keeper, orphan['id']))
        assert self.files(orphan['id']) and orphan['id'] not in self.tombstones(), 'the Grapher saw the destroy while down'
        offsets = {keeper: self.log_size(keeper) for keeper in keepers}
        self.stack.start('grapher-a')
        self.destroyed(orphan)
        # The sibling's backlog reaches the restarted Grapher; the destroyed story's never does.
        self.wait(lambda: all(f'archive_settled chunk={keeper}:{sibling["id"]}:' in self.log_since(keeper, offsets[keeper])
                              for keeper in sibling['writers']), 60)
        for keeper in keepers:
            assert f'archive_transfer_start chunk={keeper}:{orphan["id"]}:' not in self.log_since(keeper, offsets[keeper])
        assert not self.files(orphan['id'])
        self.wait(lambda: all(self.serving(keeper, sibling['id']) for keeper in keepers))

        # A Grapher that recorded the tombstone and stopped before it deleted anything resumes from the manifest
        # alone (I13.11). The kill between the two steps is not reachable from outside, so the state it leaves is
        # written here: the Grapher's own Tombstoned line, files still in place, and a restart that cannot reach the
        # Catalog.
        resume = cases['resume']
        self.stack.stop('grapher-a')
        self.feed(resume, 2)
        self.release(resume)
        self.wait(lambda: all(self.retained(keeper, resume['id']) for keeper in keepers))
        self.call('DestroyStory', dict(story_id=resume['id']))
        for keeper in keepers:
            self.wait(lambda: self.freed(keeper, resume['id']))
        assert self.files(resume['id']) and resume['id'] not in self.tombstones()
        with open(self.manifest(), 'a') as log:
            log.write(json.dumps(dict(story=resume['id'], tombstoned=True), sort_keys=True, separators=(',', ':')) + '\n')
            log.flush()
            os.fsync(log.fileno())
        node, _, config = self.stack.services['grapher-a']
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            unreachable = '127.0.0.1:' + str(probe.getsockname()[1])
            self.stack.write('grapher-a', node, dict(config, visor_internal=unreachable))
            self.stack.start('grapher-a')
            self.wait(lambda: not self.files(resume['id']))
        self.stack.stop('grapher-a')
        self.stack.write('grapher-a', node, config)
        self.stack.start('grapher-a')
        self.destroyed(resume)

        # A Keeper that is cut off from the Visors while the destroy commits misses the delta. A story created
        # afterwards puts a route revision above the tombstone's, so the snapshot it gets on reconnecting is
        # newer than the tombstone and only Catalog.GetStory can tell it the story is gone (W10.17).
        missed = cases['missed']
        # Keep dropped reports out of this case until the Keeper has reconciled through the Catalog.
        self.stack.stop('grapher-a')
        self.feed(missed, 2)
        self.release(missed)
        self.wait(lambda: all(self.retained(keeper, missed['id']) for keeper in keepers))
        partition_offset = self.log_size('proxy-keeper-1')
        self.stack.block('keeper-1', True)
        self.wait(lambda: 'partition applied' in self.log_since('proxy-keeper-1', partition_offset))
        self.call('DestroyStory', dict(story_id=missed['id']))
        later = int(self.call('CreateStory', dict(chronicle='destroy', name='later'))['story']['story_id'])
        self.call('Acquire', dict(story_id=later, writer_identity='later'))
        self.wait(lambda: self.freed('keeper-2', missed['id']))
        result = self.hot('keeper-1', missed['id'])
        # FetchHot checks the dropped set before the read gate that an acquisition watch reconnect closes.
        retained = result['transport'] == 0 and result['trailer'] and result['events'] > 0
        gated = result['transport'] == 14 and result['error'] == 'acquisition snapshot is not applied'
        assert retained or gated, f'keeper-1 applied the destroy while partitioned: {result}'
        self.stack.block('keeper-1', False)
        self.wait(lambda: self.freed('keeper-1', missed['id']))
        self.stack.start('grapher-a')
        self.destroyed(missed)
        self.rejoin('keeper-1', primary)

        # Destroying a chronicle frees every story in it.
        self.stack.stop('grapher-a')
        for record in pair.values():
            self.feed(record, 2)
        for record in pair.values():
            self.release(record)
        self.wait(lambda: all(self.retained(keeper, record['id']) for record in pair.values() for keeper in keepers))
        self.call('DestroyChronicle', dict(name='destroy-all'))
        for record in pair.values():
            for keeper in keepers:
                self.wait(lambda: self.freed(keeper, record['id']))
        self.stack.start('grapher-a')
        for record in pair.values():
            self.destroyed(record)
        for keeper in keepers:
            assert f'archive_transfer_start chunk={keeper}:{gone["id"]}:' not in self.log_since(keeper, after_gone[keeper])
        self.story = primary

    def run(self):
        self.call('CreateChronicle', dict(name='m8e'))
        created = self.call('CreateStory', dict(chronicle='m8e', name='failover'))
        self.story = int(created['story']['story_id'])
        self.expected[self.story] = {}
        registration = self.call('Register', dict(process=dict(process_id='integration-observer', instance='m8e',
                                 endpoint=self.player, role='PROCESS_ROLE_PLAYER')), self.internal)
        self.policy = registration['policy']
        self.wait(lambda: len([m for m in self.call('ListMembers', endpoint=self.internal, seconds=5)['members']
                              if m['process']['process_id'].startswith('keeper-')
                              and any(i.get('granted') for i in m.get('instances', []))]) == 2)
        writers = {}
        for n in range(8):
            identity = 'writer-' + str(n)
            a = self.call('Acquire', dict(story_id=self.story, writer_identity=identity))
            writers.setdefault(a['assigned_keeper']['process_id'], (identity, a))
            if len(writers) == 2:
                break
        assert len(writers) == 2
        for _, a in writers.values():
            for sequence in range(1, 9):
                self.append(a, sequence)
        self.complete()
        primary = self.story
        secondary = self.call('CreateStory', dict(chronicle='m8e', name='other-grapher'))
        self.secondary = int(secondary['story']['story_id'])
        self.story = self.secondary
        self.expected[self.story] = {}
        second_writer = self.call('Acquire', dict(story_id=self.story, writer_identity='secondary'))
        for sequence in range(1, 5):
            self.append(second_writer, sequence)
        self.complete()
        self.story = primary

        started = self.begin('Visor leader kill')
        identity, old = writers['keeper-1']
        observed = []
        def led():
            metadata = self.raw('Acquire', dict(story_id=self.story, writer_identity=identity))
            if metadata['transport'] == 0 and metadata.get('leader'):
                observed[:] = [metadata['leader']]
            return metadata if metadata['transport'] == 0 and metadata.get('leader') == 3 else None
        # Every Keeper, Grapher and Player lists visor-3 first, so its death is the case to prove.
        # A slow host can leave another replica leading; bounce that leader until visor-3 wins.
        metadata = None
        for attempt in range(4):
            try:
                metadata = self.wait(led, 8)
                break
            except RuntimeError:
                if observed:
                    self.stack.stop('visor-' + str(observed[0]))
                    self.stack.start('visor-' + str(observed[0]))
        assert metadata, 'visor-3 never became the leader'
        leader = metadata['leader']
        old = metadata['response']
        self.append(old)
        prior_incarnation = int(old['incarnation'])
        previous_revision = int(self.route()['revision'])
        self.stack.stop('visor-' + str(leader))
        follower = self.peers[leader % 3]
        self.endpoint, self.internal = follower['catalog_endpoint'], follower['internal_endpoint']
        replacement = self.wait(lambda: self.acquire(identity))
        assert int(replacement['incarnation']) > prior_incarnation, 'I9.1 incarnation reused after leader kill'
        self.append(replacement)
        assert int(self.route()['revision']) >= previous_revision
        self.complete()
        self.stack.start('visor-' + str(leader))
        self.complete()
        old = replacement
        self.mark('Visor leader kill I9.1 I12.1', started)

        started = self.begin('Keeper partition')
        before = int(self.route()['route']['epoch'])
        self.stack.block('keeper-1', True)
        route = self.wait(lambda: self.changed(before, 'keeper-1', False))
        predecessor = next(p for p in route['predecessors'] if p['keeper']['process_id'] == 'keeper-1')
        cut = hlc(route['ordering_cut'])
        # No ceiling renewal is possible; the still-running old owner must stop admitting once its
        # last granted ceiling, recorded as the predecessor's own cut, is behind the wall clock.
        passed = hlc(predecessor['own_cut'])[0] + 100_000_000
        self.wait(lambda: time.time_ns() >= passed)
        rejected = self.append(old, 2, retry=False, whole_rpc_unavailable=True)
        assert int(rejected.get('status', {}).get('code', 0)) == 14, 'I4.7 partitioned owner admitted beyond ceiling'
        self.stack.stop('keeper-1')
        def failed():
            _, c = self.read()
            return c if not c.get('complete') and c.get('reason') == 'INCOMPLETE_REASON_SOURCE_FAILED' else None
        self.wait(failed)
        self.mark('Keeper partition I4.7 I4.13 SOURCE_FAILED', started)

        started = self.begin('successor above cut')
        successor = self.acquire(identity)
        assert successor['assigned_keeper']['process_id'] == 'keeper-2'
        assert int(successor['incarnation']) > int(old['incarnation'])
        result = self.append(successor)
        assert hlc(result['assigned_hlc']) > cut, 'I4.11 successor admitted below cut'
        self.complete(start=hlc(result['assigned_hlc']))
        self.wait(failed)
        self.mark('successor above cut I4.9 I4.11', started)

        started = self.begin('predecessor drain to archive')
        self.stack.block('keeper-1', False)
        self.stack.start('keeper-1')
        def drained():
            current = self.route()
            if (not any(p['instance'] == predecessor['instance'] for p in current.get('predecessors', []))
                    and hlc(current.get('archived_below', {})) >= hlc(predecessor['own_cut'])):
                return current
            raise RuntimeError(json.dumps(dict(route=current, members=self.state().get('members'))))
        self.wait(drained)
        self.complete()
        self.stack.stop('keeper-1')
        self.complete()
        self.stack.start('keeper-1')
        self.mark('predecessor drain to archive I4.14 I12.1', started)

        started = self.begin('ping-pong transition budget')
        owner = 'keeper-2'
        unavailable = 0
        for turn in range(6):
            other = 'keeper-1' if owner == 'keeper-2' else 'keeper-2'
            before = int(self.route()['route']['epoch'])
            self.admin('JoinKeeper', other)
            joined = self.wait(lambda: self.changed(before, other, True))
            self.wait(lambda: self.applied(other, int(joined['revision'])))
            until = int(joined.get('physical_floor_ns', 0)) - int(self.policy['acceptance_budget_ns'])
            unavailable += self.deferral(other, until, turn)
            before = int(self.route()['route']['epoch'])
            self.admin('DrainKeeper', owner)
            route = self.wait(lambda: self.changed(before, owner, False))
            self.wait(lambda: self.applied(other, int(route['revision'])))
            a = self.acquire(identity)
            assert a['assigned_keeper']['process_id'] == other
            attempt_time, r, end = self.settled(a)
            code = int(r.get('status', {}).get('code', 0))
            if code == 0:
                assert end + 100_000_000 >= until, 'I4.8 admitted before the physical floor was within budget'
                assert hlc(r['assigned_hlc'])[0] <= attempt_time + int(self.policy['hlc_budget_ns']) + 1_000_000_000
            else:
                assert code == 14, r
            r = self.append(a)
            assert hlc(r['assigned_hlc']) > hlc(route['ordering_cut'])
            assert hlc(r['assigned_hlc'])[0] <= time.time_ns() + int(self.policy['hlc_budget_ns']) + 1_000_000_000
            self.complete()
            owner = other
        assert unavailable > 0, 'I4.8 ping-pong never deferred admission'
        self.mark('ping-pong transition budget I4.8 I4.11', started)

        started = self.begin('join with stale Player')
        other = 'keeper-1' if owner == 'keeper-2' else 'keeper-2'
        # Player retains its last cached Route while join advances the epoch.
        self.complete()
        self.admin('JoinKeeper', other)
        self.complete()
        primary = self.story
        self.story = self.secondary
        self.complete()
        self.story = primary
        self.mark('join with stale Player I4.9 I6.11', started)

        started = self.begin('story and chronicle destroy')
        if hasattr(self.stack, 'cluster'):
            print('SKIP story and chronicle destroy: the archive is on another host', flush=True)
        else:
            self.destroy()
            self.mark('story and chronicle destroy W10.5 W10.17 I13.11 I6.7', started)

        started = self.begin('abandonment')
        # Keep a new acknowledged event unsettled, so the instance proof leaves a real gap.
        self.stack.stop('grapher-a')
        if 'grapher-b' in self.stack.services:
            self.stack.stop('grapher-b')
        a = None
        for n in range(8):
            candidate = self.acquire('abandon-' + str(n))
            if candidate['assigned_keeper']['process_id'] == other:
                a = candidate
                break
        assert a is not None
        last = self.append(a)
        self.stack.stop(other)
        self.admin('AbandonKeeper', other)
        route = self.route()
        assert any(hlc(r.get('start', {})) <= hlc(last['assigned_hlc']) < hlc(r['end'])
                   for r in route.get('abandoned', [])), 'I4.15 abandonment omitted unsettled acknowledged event'
        self.wait(failed)
        self.stack.start('grapher-a')
        if 'grapher-b' in self.stack.services:
            self.stack.start('grapher-b')
        self.stack.start(other)
        self.wait(failed)
        # Outside the abandoned range, the successor remains available and correctly ordered.
        survivor = self.acquire(identity)
        new = self.append(survivor)
        assert hlc(new['assigned_hlc']) > hlc(route['ordering_cut'])
        self.complete(start=hlc(new['assigned_hlc']))
        self.wait(failed)
        self.mark('abandonment I4.15 SOURCE_FAILED persists', started)
        print('PASS all acknowledged DURABLE identities HLCs payloads and order verified in complete ranges', flush=True)
