from physical_claim import PhysicalClaim, UNCERTAINTY_NS
from scenario import event, hlc, wire


class PrefixClaim(PhysicalClaim):
    def prefix_read(self, axis, end, claimed):
        request = dict(prefix='dynamic-prefix')
        if axis == 'hlc':
            request['hlc'] = dict(start=wire((0, 0)), end=wire(end))
        else:
            request['physical'] = dict(start_ns='0', end_ns=str(end))
        result = self.raw('Read', request, self.player)
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
        assert actual == sorted(set(actual)), 'I7.7 prefix events are not ordered and unique'
        assert all(e in claimed for e in actual), f'prefix {axis} returned an unexpected event: {actual}'
        if not completion.get('complete', False):
            raise RuntimeError(f'prefix {axis} incomplete: {completion}')
        assert actual == claimed, f'complete prefix {axis} changed its event set: {actual} != {claimed}'
        assert int(completion.get('catalog_revision', 0)) > 0, 'I6.15 missing Catalog revision'
        return completion

    def create(self, name, keeper):
        created = self.call('CreateStory', dict(chronicle='dynamic-prefix', name=name))
        self.story = int(created['story']['story_id'])
        self.wait(lambda: any(int(r['story_id']) == self.story for r in self.state().get('routes', [])),
                  context=f'{name} Route published')
        request = self.acquire_request(name)
        request['preferred_keeper_process_id'] = keeper
        writer = self.call_acquire(request)
        assert writer['assigned_keeper']['process_id'] == keeper, writer
        return writer

    def run(self):
        self.wait(lambda: self.registered(('keeper-1', 'keeper-2')), context='bootstrap Keepers register')
        self.call('CreateChronicle', dict(name='dynamic-prefix'))
        started = self.begin('complete prefix claims on both axes')
        acknowledged = []
        for name, keeper in (('before-a', 'keeper-1'), ('before-b', 'keeper-2')):
            writer = self.create(name, keeper)
            for sequence in (1, 2):
                result = self.append_at(writer, sequence)
                assert int(result.get('status', {}).get('code', 0)) == 0, result
                acknowledged.append(result)
        claimed = sorted(r['event'] for r in acknowledged)
        last = max(r['event'][:2] for r in acknowledged)
        ends = dict(hlc=(last[0], last[1] + 1),
                    physical=max(r['lo'] for r in acknowledged) + 2 * UNCERTAINTY_NS + 1)
        revisions = {}
        for axis, end in ends.items():
            completion = self.wait(lambda: self.prefix_read(axis, end, claimed), 60)
            revisions[axis] = int(completion['catalog_revision'])
        self.mark('complete prefix claims on both axes I6.15', started)

        started = self.begin('Keeper join and child story creation')
        previous = int(self.route()['route']['epoch'])
        self.stack.start('keeper-3')
        self.wait(lambda: self.registered(('keeper-3',)), context='new Keeper registers and receives a ceiling')
        self.admin('JoinKeeper', 'keeper-3')
        joined = self.wait(lambda: self.changed(previous, 'keeper-3', True), context='new Keeper joins the Route')
        self.wait(lambda: self.applied('keeper-3', int(joined['revision'])), context='new Keeper applies the join')
        writer = self.create('after', 'keeper-3')
        later = self.append_at(writer, 1)
        assert int(later.get('status', {}).get('code', 0)) == 0, later
        assert later['event'][:2] >= ends['hlc'], 'I6.16 new child admitted below the HLC claim'
        assert later['lo'] >= ends['physical'], 'I6.16 new child admitted below the physical claim'
        self.mark('new child appends on the joined Keeper above both claims I6.16', started)

        started = self.begin('prefix claims repeat after the join and creation')
        for axis, end in ends.items():
            completion = self.wait(lambda: self.prefix_read(axis, end, claimed), 60)
            assert int(completion['catalog_revision']) > revisions[axis], 'prefix did not resolve the new revision'
        expanded = sorted(claimed + [later['event']])
        expanded_ends = dict(hlc=(later['event'][0], later['event'][1] + 1),
                             physical=later['lo'] + 2 * UNCERTAINTY_NS + 1)
        for axis, end in expanded_ends.items():
            self.wait(lambda: self.prefix_read(axis, end, expanded), 60)
        self.mark('same complete event sets on both axes and new child visible I6.15 I6.16', started)
        print('PASS dynamic prefix claims survive a Keeper join and child story creation on both axes', flush=True)
