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
                             assigned_keeper=dict(endpoint='keeper'))
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

    @patch('scenario.time.monotonic', side_effect=[0, 30])
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
