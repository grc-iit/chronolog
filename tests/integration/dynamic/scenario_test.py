import os
import subprocess
import unittest
from unittest.mock import Mock, patch

import run
from scenario import Scenario


class SettledTest(unittest.TestCase):
    def setUp(self):
        self.scenario = object.__new__(Scenario)
        self.scenario.story = 7
        self.acquired = dict(writer_id=3, incarnation=2, route=dict(epoch=4),
                             assigned_keeper=dict(endpoint='keeper'), hold_deadline=float('inf'))
        self.pending = dict(transport=14, error='acquisition snapshot is not applied', response={})

    @patch('scenario.time.sleep')
    def test_acquisition_refusals_preserve_the_transition_answer_and_event_id(self, sleep):
        deferred = dict(status=dict(code=14, message='transition exceeds clock budget'))
        requests = []
        responses = iter([self.pending, dict(transport=0, response=dict(results=[dict(status=dict(code=9))])),
                          dict(transport=0, response=dict(results=[deferred]))])

        def respond(op, request, endpoint):
            item = request['items'][0]
            requests.append((request['story_id'], request['epoch'], item['writer_id'],
                             item['incarnation'], item['sequence'], item['envelope']['payload']))
            return next(responses)

        self.scenario.raw = respond
        _, result, _ = self.scenario.settled(self.acquired)
        self.assertEqual(result, deferred)
        self.assertEqual(len(requests), 3)
        self.assertEqual(requests, [requests[0]] * 3)
        self.assertEqual(sleep.call_count, 2)

    def test_unrelated_transport_unavailable_fails(self):
        self.scenario.raw = Mock(return_value=dict(transport=14, error='connection lost', response={}))
        with self.assertRaisesRegex(RuntimeError, 'connection lost'):
            self.scenario.settled(self.acquired)
        self.scenario.raw.assert_called_once()

    def test_item_unavailable_is_an_answer_even_with_the_same_message(self):
        item = dict(status=dict(code=14, message=self.pending['error']))
        self.scenario.raw = Mock(return_value=dict(transport=0, response=dict(results=[item])))
        self.assertEqual(self.scenario.settled(self.acquired)[1], item)
        self.scenario.raw.assert_called_once()

    # Samples: the settle deadline, the hold check inside append, then the expired deadline.
    @patch('scenario.time.monotonic', side_effect=[0, 0, 30])
    def test_snapshot_wait_keeps_the_existing_deadline(self, monotonic):
        self.scenario.raw = Mock(return_value=self.pending)
        with self.assertRaisesRegex(AssertionError, 'acquisition admission never became ready'):
            self.scenario.settled(self.acquired)
        self.scenario.raw.assert_called_once()

    def test_append_without_transport_opt_in_still_fails(self):
        self.scenario.raw = Mock(return_value=self.pending)
        with self.assertRaisesRegex(RuntimeError, 'acquisition snapshot is not applied'):
            self.scenario.append(self.acquired, retry=False)


class CatalogReadTest(unittest.TestCase):
    def setUp(self):
        self.scenario = object.__new__(Scenario)
        self.scenario.internal = 'visor-internal'
        self.scenario.story = 7
        self.route = dict(story_id=7, route=dict(epoch=4))
        self.state = dict(routes=[self.route], members=[dict(process=dict(process_id='keeper-a'),
                                                          applied_route_revision=4)])
        self.unavailable = dict(transport=14, error='no leader lease')

    @patch('scenario.time.sleep')
    def test_read_helpers_retry_a_lapsed_leader_lease(self, sleep):
        for read, expected in ((self.scenario.state, self.state),
                               (self.scenario.route, self.route),
                               (lambda: self.scenario.applied('keeper-a', 4), True)):
            with self.subTest(read=read):
                sleep.reset_mock()
                self.scenario.raw = Mock(side_effect=[self.unavailable,
                                                     dict(transport=0, response=self.state)])
                self.assertEqual(read(), expected)
                self.assertEqual(self.scenario.raw.call_count, 2)
                for call in self.scenario.raw.call_args_list:
                    self.assertEqual(call.args, ('ListMembers', None, 'visor-internal'))
                sleep.assert_called_once_with(.1)

    @patch('scenario.time.sleep')
    def test_item_unavailable_uses_the_same_retry_path(self, sleep):
        self.scenario.raw = Mock(side_effect=[dict(transport=0, response=dict(status=dict(code=14))),
                                             dict(transport=0, response=self.state)])
        self.assertEqual(self.scenario.state(), self.state)
        self.assertEqual(self.scenario.raw.call_count, 2)

    def test_non_retryable_catalog_failure_is_not_hidden(self):
        self.scenario.raw = Mock(return_value=dict(transport=7, error='permission denied'))
        with self.assertRaisesRegex(AssertionError, 'permission denied'):
            self.scenario.state()
        self.scenario.raw.assert_called_once()

    @patch('scenario.time.sleep')
    @patch('scenario.time.monotonic', side_effect=[0, 0, 30])
    def test_catalog_retry_keeps_the_existing_wait_bound(self, monotonic, sleep):
        self.scenario.raw = Mock(return_value=self.unavailable)
        with self.assertRaisesRegex(RuntimeError, 'ListMembers stayed UNAVAILABLE for 30s'):
            self.scenario.state()
        self.scenario.raw.assert_called_once()


class PingPongDrainTest(unittest.TestCase):
    def setUp(self):
        self.scenario = object.__new__(Scenario)
        self.joined = dict(revision=164, route=dict(epoch=7, keepers=[dict(process_id='keeper-1'),
                                                                   dict(process_id='keeper-2')]))
        self.drained = dict(revision=168, route=dict(epoch=8, keepers=[dict(process_id='keeper-1')]))
        self.scenario.admin = Mock(return_value=dict(routes=[self.drained]))

    def test_silence_detection_during_deferral_does_not_require_a_second_drain(self):
        # The owner's automatic drain committed before the probe returned. The manual drain is idempotent.
        self.scenario.route = Mock(return_value=self.drained)
        self.assertEqual(self.scenario.drain_after_join(self.joined, 'keeper-2'), self.drained)
        self.scenario.admin.assert_called_once_with('DrainKeeper', 'keeper-2')
        self.scenario.route.assert_called_once()

    @patch('scenario.time.sleep')
    def test_wait_still_requires_owner_removal_and_epoch_advancement(self, sleep):
        newer_but_present = dict(revision=166, route=dict(epoch=8, keepers=self.joined['route']['keepers']))
        removed_without_advancement = dict(revision=167, route=dict(epoch=7, keepers=self.drained['route']['keepers']))
        self.scenario.route = Mock(side_effect=[newer_but_present, removed_without_advancement, self.drained])
        self.assertEqual(self.scenario.drain_after_join(self.joined, 'keeper-2'), self.drained)
        self.assertEqual(self.scenario.route.call_count, 3)
        self.assertEqual(sleep.call_count, 2)

    @patch('scenario.time.sleep')
    @patch('scenario.time.monotonic', side_effect=[0, 0, 30])
    def test_failed_wait_names_the_condition_with_the_existing_deadline(self, monotonic, sleep):
        self.scenario.route = Mock(return_value=self.joined)
        with self.assertRaisesRegex(RuntimeError, 'drain keeper-2 after joined epoch 7'):
            self.scenario.drain_after_join(self.joined, 'keeper-2')
        self.scenario.route.assert_called_once()


class AcquireTest(unittest.TestCase):
    def setUp(self):
        self.scenario = object.__new__(Scenario)
        self.scenario.story = 7
        self.granted = dict(writer_id=3, incarnation=2,
                            lease=dict(duration_ns=str(Scenario.MAX_LEASE_NS), remaining_ns=str(Scenario.MAX_LEASE_NS)))
        self.leaseless = dict(transport=14, error='no Raft leader', leader=3)

    @patch('scenario.time.sleep')
    def test_refusal_before_apply_retries_the_same_request(self, sleep):
        self.scenario.raw = Mock(side_effect=[self.leaseless, dict(transport=14, error='catalog is overloaded'),
                                             dict(transport=0, response=self.granted)])
        self.assertEqual(self.scenario.acquire('main-writer'), self.granted)
        calls = self.scenario.raw.call_args_list
        self.assertEqual(calls, [calls[0]] * 3)
        op, request = calls[0].args
        self.assertEqual(op, 'Acquire')
        self.assertEqual(len(request.pop('acquire_request_id')), 32)
        self.assertEqual(request, dict(story_id=7, writer_identity='main-writer',
                                       lease_duration_ns=Scenario.MAX_LEASE_NS))
        self.assertEqual(sleep.call_count, 2)

    def test_each_logical_acquire_mints_one_id_and_a_frozen_request_keeps_it(self):
        self.scenario.raw = Mock(return_value=dict(transport=0, response=self.granted))
        frozen = self.scenario.acquire_request('main-writer', expected=2, takeover=True)
        self.scenario.acquire('main-writer', request=frozen)
        self.scenario.acquire('main-writer', request=frozen)
        self.scenario.acquire('main-writer')
        ids = [call.args[1]['acquire_request_id'] for call in self.scenario.raw.call_args_list]
        self.assertEqual(ids[0], ids[1])
        self.assertNotEqual(ids[1], ids[2])
        self.assertEqual(frozen['expected_prior_incarnation'], 2)
        self.assertTrue(frozen['takeover'])

    def test_a_hold_past_the_grant_fails(self):
        self.scenario.raw = Mock(return_value=dict(transport=0, response=dict(self.granted, lease=dict(
            duration_ns=str(Scenario.MAX_LEASE_NS), remaining_ns='0'))))
        acquired = self.scenario.acquire('main-writer')
        with self.assertRaisesRegex(AssertionError, 'past its grant'):
            self.scenario.append(acquired)

    def test_a_grant_below_the_maximum_lease_fails(self):
        self.scenario.raw = Mock(return_value=dict(transport=0, response=dict(self.granted, lease=dict(
            duration_ns='300000000000', remaining_ns='300000000000'))))
        with self.assertRaisesRegex(AssertionError, 'maximum lease'):
            self.scenario.acquire('main-writer')

    def test_other_unavailable_is_not_retried(self):
        self.scenario.raw = Mock(return_value=dict(transport=14, error='Socket closed'))
        with self.assertRaisesRegex(RuntimeError, 'Socket closed'):
            self.scenario.acquire('main-writer')
        self.scenario.raw.assert_called_once()

    def test_domain_refusal_is_the_answer(self):
        self.scenario.raw = Mock(return_value=dict(transport=0, response=dict(status=dict(code=14))))
        with self.assertRaisesRegex(RuntimeError, 'Acquire'):
            self.scenario.acquire('main-writer')
        self.scenario.raw.assert_called_once()

    @patch('scenario.time.sleep')
    @patch('scenario.time.monotonic', side_effect=[0, 30])
    def test_retry_keeps_the_existing_wait_bound(self, monotonic, sleep):
        self.scenario.raw = Mock(return_value=self.leaseless)
        with self.assertRaisesRegex(RuntimeError, 'no Raft leader'):
            self.scenario.acquire('main-writer')
        self.scenario.raw.assert_called_once()


class LocalStopTest(unittest.TestCase):
    def setUp(self):
        self.stack = object.__new__(run.Local)
        self.stack.processes = {}
        self.stack.groups = {}
        self.stack.services = {}

    def test_stop_returns_only_after_every_group_member_exits(self):
        # The wrapper's child keeps running after the wrapper is reaped, like the Keeper behind timeout.
        process = subprocess.Popen(['sh', '-c', 'sleep 30 & sleep 30 & wait'], start_new_session=True)
        self.stack.processes['keeper-1'] = (process, open(os.devnull, 'wb'))
        self.stack.stop('keeper-1')
        with self.assertRaises(ProcessLookupError):
            os.killpg(process.pid, 0)
        self.assertEqual(self.stack.groups, {})

    @patch('run.time.sleep')
    @patch('run.os.killpg')
    def test_stop_polls_the_group_after_the_wrapper_is_reaped(self, killpg, sleep):
        process = Mock(pid=4321)
        killpg.side_effect = [None, None, None, ProcessLookupError()]
        self.stack.processes['keeper-1'] = (process, Mock())
        self.stack.stop('keeper-1')
        process.wait.assert_called_once()
        self.assertEqual(killpg.call_args_list[1:], [((4321, 0),)] * 3)
        self.assertEqual(sleep.call_count, 2)
        self.assertEqual(self.stack.groups, {})

    @patch('run.time.sleep')
    @patch('run.time.monotonic', side_effect=[0, 0, run.STOP_SECONDS])
    @patch('run.os.killpg')
    def test_start_refuses_a_role_whose_group_outlived_the_stop_bound(self, killpg, monotonic, sleep):
        self.stack.processes['keeper-1'] = (Mock(pid=4321), Mock())
        self.stack.stop('keeper-1')
        self.assertEqual(self.stack.groups, {'keeper-1': 4321})
        with self.assertRaisesRegex(RuntimeError, 'keeper-1 previous process group 4321 is still alive'):
            self.stack.start('keeper-1')
        killpg.side_effect = ProcessLookupError()
        with self.assertRaises(KeyError):  # past the group check, at the unconfigured service
            self.stack.start('keeper-1')
        self.assertEqual(self.stack.groups, {})


if __name__ == '__main__':
    unittest.main()
