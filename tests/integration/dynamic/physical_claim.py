import base64
import time

from scenario import Scenario, event, hlc

# Writer clock bound on every event this scenario appends, so each event has a finite interval (I8.9).
UNCERTAINTY_NS = 1_000_000


class PhysicalClaim(Scenario):
    """I8.11 survivor clause across a real Keeper change. A physical-axis Read reports complete through P; keeper-1
    is SIGKILLed and leaves the Route, keeper-3, new to the Route, joins; the survivor and the new member refuse
    appends that would land below P, and the same Read repeats with the same events and complete=true."""

    def append_at(self, acquired, sequence, physical_ns=None):
        # A Synced reading at physical_ns, or at the wall clock of each attempt. UNAVAILABLE and FAILED_PRECONDITION
        # before a definitive answer are a Keeper not ready for the writer yet (I4.8, W10.14); the same EventId is
        # resent within the bound.
        self.hold(acquired)
        payload = base64.b64encode(
            f"{self.story}:{acquired['writer_id']}:{acquired['incarnation']}:{sequence}".encode()).decode()
        item = dict(writer_id=acquired['writer_id'], incarnation=acquired['incarnation'], sequence=sequence,
                    physical=dict(uncertainty_ns=str(UNCERTAINTY_NS), status='CLOCK_STATUS_SYNCED'),
                    envelope=dict(payload=payload))
        request = dict(story_id=self.story, epoch=acquired['route']['epoch'], durability='DURABILITY_DURABLE',
                       items=[item])
        deadline = time.monotonic() + 30
        while True:
            item['physical']['physical_ns'] = str(time.time_ns() if physical_ns is None else physical_ns)
            response = self.raw('Append', request, acquired['assigned_keeper']['endpoint'])
            if response['transport'] not in (0, 14):
                raise RuntimeError(str(response))
            result = response['response']['results'][0] if response['transport'] == 0 else {}
            code = int(result.get('status', {}).get('code', 0)) if result else 14
            if code not in (9, 14):
                if code == 0:
                    assert result['achieved_durability'] == 'DURABILITY_DURABLE', result
                    result['event'] = (*hlc(result['assigned_hlc']), self.story, int(acquired['writer_id']),
                                       int(acquired['incarnation']), sequence, payload)
                    result['lo'] = int(item['physical']['physical_ns']) - UNCERTAINTY_NS
                return result
            assert time.monotonic() < deadline, f'append never got a definitive answer: {response}'
            time.sleep(.05)

    def physical_read(self, start, end):
        result = self.raw('Read', dict(story_id=self.story, physical=dict(start_ns=str(start), end_ns=str(end))),
                          self.player)
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
        return actual, completion

    def same_claim(self, start, end, claimed, step):
        # Any complete answer for the claimed range is the claim itself (I8.11); an incomplete one is retryable.
        actual, completion = self.physical_read(start, end)
        if not completion.get('complete', False):
            raise RuntimeError(f'physical Read {step} incomplete: {completion}')
        assert actual == claimed, f'I8.11 complete physical Read changed {step}: {actual} != {claimed}'
        return completion

    def probe_claim(self, start, end, claimed, step):
        try:
            self.same_claim(start, end, claimed, step)
        except RuntimeError as error:
            print(error, flush=True)

    def writer_on(self, keeper, name):
        for n in range(16):
            acquired = self.acquire(f'{name}-{keeper}-{n}')
            if acquired['assigned_keeper']['process_id'] == keeper:
                return acquired
        raise AssertionError(f'no writer was assigned to {keeper}')

    def registered(self, keepers):
        return len([m for m in self.state()['members'] if m['process']['process_id'] in keepers
                    and any(i.get('granted') for i in m.get('instances', []))]) == len(keepers)

    def run(self):
        # The first Route carries the physical policy only if its Keepers registered with it (I8.12).
        self.wait(lambda: self.registered(('keeper-1', 'keeper-2')), context='keeper-1 and keeper-2 register')
        self.call('CreateChronicle', dict(name='i811'))
        self.story = int(self.call('CreateStory', dict(chronicle='i811', name='claim'))['story']['story_id'])
        self.expected[self.story] = {}
        self.wait(lambda: any(int(r['story_id']) == self.story for r in self.state().get('routes', [])))
        assert self.route().get('physical_policy'), f'story has no physical policy (I8.12): {self.route()}'

        started = self.begin('physical claim')
        acknowledged = []
        for keeper in ('keeper-1', 'keeper-2'):
            writer = self.writer_on(keeper, 'claim')
            for sequence in range(1, 5):
                result = self.append_at(writer, sequence)
                assert int(result.get('status', {}).get('code', 0)) == 0, result
                acknowledged.append(result)
        start = min(r['lo'] for r in acknowledged)
        end = max(r['lo'] for r in acknowledged) + 2 * UNCERTAINTY_NS + 1
        claimed = sorted(r['event'] for r in acknowledged)
        # Complete once every Route Keeper's physical frontier passes `end`, an acceptance window after it.
        self.wait(lambda: self.same_claim(start, end, claimed, 'before the Keeper change'), 60)
        self.mark('physical claim I6.10', started)

        started = self.begin('Keeper change')
        before = int(self.route()['route']['epoch'])
        self.stack.stop('keeper-1')
        route = self.wait(lambda: self.changed(before, 'keeper-1', False), context='keeper-1 leaves the Route')
        predecessor = next(p for p in route['predecessors'] if p['keeper']['process_id'] == 'keeper-1')
        self.probe_claim(start, end, claimed, 'after keeper-1 left')
        self.stack.start('keeper-3')
        self.wait(lambda: self.registered(('keeper-3',)), context='keeper-3 registers')
        before = int(self.route()['route']['epoch'])
        self.admin('JoinKeeper', 'keeper-3')
        joined = self.wait(lambda: self.changed(before, 'keeper-3', True), context='keeper-3 joins the Route')
        self.wait(lambda: self.applied('keeper-3', int(joined['revision'])), context='keeper-3 applies its join')
        self.probe_claim(start, end, claimed, 'after keeper-3 joined')
        self.mark('Keeper change I4.9', started)

        started = self.begin('later appends above the claim')
        for keeper in ('keeper-2', 'keeper-3'):
            writer = self.writer_on(keeper, 'later')
            # An interval around end - 1 intersects [start, end): accepting it would change the claimed answer.
            refused = self.append_at(writer, 1, end - 1)
            code = int(refused.get('status', {}).get('code', 0))
            assert code == 11, f'I8.11 {keeper} answered {code}, not OUT_OF_RANGE, for an event below P={end}: {refused}'
            result = self.append_at(writer, 2)
            assert int(result.get('status', {}).get('code', 0)) == 0, result
            assert result['lo'] >= end, f'I8.11 {keeper} admitted an event at or below P={end}: {result}'
        self.probe_claim(start, end, claimed, 'after later appends')
        self.mark('later appends above the claim I8.9 I8.10 I8.11', started)

        started = self.begin('claim repeats')
        # The killed Keeper's span stays a source until its restarted instance drains it to the archive (I4.13).
        self.stack.start('keeper-1')
        def drained():
            current = self.route()
            return current if (not any(p['instance'] == predecessor['instance']
                                       for p in current.get('predecessors', []))
                               and hlc(current.get('archived_below', {})) >= hlc(predecessor['own_cut'])) else None
        self.wait(drained, 90, context='keeper-1 drains its predecessor span')
        self.wait(lambda: self.same_claim(start, end, claimed, 'after the drain'), 60)
        self.mark('claim repeats complete with the same events I8.11', started)
        print('PASS physical claim survived a Keeper kill and a new member', flush=True)
