import base64
import json
import os
import select
import signal
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
        return self.rpc('ListMembers', endpoint=self.internal)

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
                    return dict(status=dict(code=14))
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

    def run(self):
        self.wait(lambda: self.rpc('CreateChronicle', dict(name='m8e')))
        created = self.rpc('CreateStory', dict(chronicle='m8e', name='failover'))
        self.story = int(created['story']['story_id'])
        self.expected[self.story] = {}
        registration = self.rpc('Register', dict(process=dict(process_id='integration-observer', instance='m8e',
                                endpoint=self.player, role='PROCESS_ROLE_PLAYER')), self.internal)
        self.policy = registration['policy']
        self.wait(lambda: len([m for m in self.state()['members']
                              if m['process']['process_id'].startswith('keeper-')
                              and any(i.get('granted') for i in m.get('instances', []))]) == 2)
        writers = {}
        for n in range(8):
            identity = 'writer-' + str(n)
            a = self.acquire(identity)
            writers.setdefault(a['assigned_keeper']['process_id'], (identity, a))
            if len(writers) == 2:
                break
        assert len(writers) == 2
        for _, a in writers.values():
            for sequence in range(1, 9):
                self.append(a, sequence)
        self.complete()
        primary = self.story
        secondary = self.rpc('CreateStory', dict(chronicle='m8e', name='other-grapher'))
        self.secondary = int(secondary['story']['story_id'])
        self.story = self.secondary
        self.expected[self.story] = {}
        second_writer = self.acquire('secondary')
        for sequence in range(1, 5):
            self.append(second_writer, sequence)
        self.complete()
        self.story = primary

        started = self.begin('Visor leader kill')
        identity, old = writers['keeper-1']
        def led():
            metadata = self.raw('Acquire', dict(story_id=self.story, writer_identity=identity))
            return metadata if metadata['transport'] == 0 and metadata.get('leader') == 3 else None
        # Every Keeper, Grapher and Player is configured with visor-3 alone, so its death is the case to prove.
        metadata = self.wait(led, 20)
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
        # No ceiling renewal is possible; the still-running old owner must stop admitting.
        time.sleep(int(self.policy['ceiling_ahead_ns']) / 1e9 + .3)
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
            self.admin('JoinKeeper', other)
            before = int(self.route()['route']['epoch'])
            self.admin('DrainKeeper', owner)
            route = self.wait(lambda: self.changed(before, owner, False))
            self.wait(lambda: self.applied(other, int(route['revision'])))
            a = self.acquire(identity)
            assert a['assigned_keeper']['process_id'] == other
            time.sleep(.2)
            attempt_time = time.time_ns()
            r = self.append(a, retry=False)
            if int(r.get('status', {}).get('code', 0)) == 14:
                unavailable += 1
            else:
                assert int(r.get('status', {}).get('code', 0)) == 0, r
                assert hlc(r['assigned_hlc'])[0] <= attempt_time + int(self.policy['hlc_budget_ns']) + 1_000_000_000
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
