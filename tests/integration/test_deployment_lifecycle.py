#!/usr/bin/env python3
"""Pure controller regressions. Scripted peers do not prove native retirement."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest import mock

MODULE = Path(__file__).resolve().parents[2] / 'tools/deploy/deployment_lifecycle.py'
spec = importlib.util.spec_from_file_location('deployment_lifecycle', MODULE)
lifecycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lifecycle)


class WorkerFailureDetails(unittest.TestCase):
    def test_nonzero_worker_preserves_structured_reason_and_exit_status(self):
        child = mock.Mock(returncode=27)
        child.communicate.return_value = ('{"status":-27,"reason":"process identity not yet verifiable"}\n', '')
        child.poll.return_value = 27
        with mock.patch.object(lifecycle.subprocess, 'Popen', return_value=child):
            with self.assertRaisesRegex(RuntimeError, 'worker exit 27: process identity not yet verifiable'):
                lifecycle.call_worker('local', '/tmp/owned-test', 'inventory', lifecycle.time.monotonic()+1)
        child.kill.assert_not_called()

    def test_nonjson_worker_preserves_bounded_stderr_and_exit_status(self):
        child = mock.Mock(returncode=255)
        child.communicate.return_value = ('', 'x'*2048+'SSH connection refused')
        child.poll.return_value = 255
        with mock.patch.object(lifecycle.subprocess, 'Popen', return_value=child):
            with self.assertRaises(RuntimeError) as caught:
                lifecycle.call_worker('remote-node', '/tmp/owned-test', 'inventory', lifecycle.time.monotonic()+1)
        self.assertIn('worker exit 255:', str(caught.exception))
        self.assertIn('SSH connection refused', str(caught.exception))
        self.assertLess(len(str(caught.exception)), 1100)
        child.kill.assert_not_called()


class LaunchObservation(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='chronolog-launch-observation-')
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        self.config = self.work/'service.json'
        self.config.write_text('retained configuration')
        self.specification = dict(component='player', executable='/test/chrono-player',
            config=str(self.config), output=str(self.work/'launch.log'),
            budget_ms=80, deployment_id='deployment-A', launch_id='original-launch')
        self.observed = dict(pid=123, uid=lifecycle.os.geteuid(), executable='/test/chrono-player',
            argv=['/test/chrono-player','--config',str(self.config)], start_ticks='7', boot_id='boot-A')
        self.manifest = self.work/'monitor/lifecycle/deployment.json'
        lifecycle.atomic_json(self.manifest, dict(id='deployment-A', state='starting',records=[],keepers=[],
            pending_launch={'route':'local','specification':self.specification}))
        Path(str(self.config)+'.lifecycle.json').write_text(json.dumps(self.observed | dict(
            config=str(self.config), host=lifecycle.socket.gethostname(),
            deployment_id='deployment-A', state='Running', status=0)))
        self.clock = 0.0
        self.child = mock.Mock(pid=123)
        self.child.poll.return_value = None

    def launch(self, observation):
        def sleep(seconds):
            self.clock += seconds
        with mock.patch.object(lifecycle.subprocess,'Popen',return_value=self.child) as launch, \
             mock.patch.object(lifecycle,'process_identity',side_effect=observation), \
             mock.patch.object(lifecycle.time,'monotonic',side_effect=lambda:self.clock), \
             mock.patch.object(lifecycle.time,'sleep',side_effect=sleep), \
             mock.patch.dict(lifecycle.os.environ,{'CHRONOLOG_DEPLOYMENT_ID':'deployment-A'}):
            try:
                return lifecycle.launch(self.work,self.specification)
            finally:
                launch.assert_called_once()
                self.child.kill.assert_not_called()
                self.child.terminate.assert_not_called()

    def test_empty_exec_arguments_later_verify_same_original_child(self):
        calls = 0
        def observation(pid):
            nonlocal calls
            calls += 1
            return self.observed | {'argv':[]} if calls == 1 else self.observed
        result = self.launch(observation)
        self.assertEqual(result['pid'],123)
        self.assertEqual(result['start_ticks'],'7')
        self.assertEqual(self.clock,.02)
        manifest = json.loads(self.manifest.read_text())
        self.assertNotIn('pending_launch',manifest)
        self.assertEqual(len(manifest['records']),1)

    def test_permanently_empty_arguments_expire_original_budget_without_relaunch(self):
        with self.assertRaisesRegex(TimeoutError,'startup budget expired'):
            self.launch(lambda pid:self.observed | {'argv':[]})
        self.assertAlmostEqual(self.clock,.08)
        manifest = json.loads(self.manifest.read_text())
        self.assertIn('pending_launch',manifest)
        self.assertEqual(manifest['records'],[])

    def test_nonempty_wrong_config_is_never_accepted(self):
        with self.assertRaisesRegex(RuntimeError,'config identity is unverified'):
            self.launch(lambda pid:self.observed | {'argv':['/test/chrono-player','--config','/different/config']})
        self.assertEqual(self.clock,0)
        self.assertIn('pending_launch',json.loads(self.manifest.read_text()))

    def test_changed_lifetime_after_empty_observation_is_never_recorded(self):
        calls = 0
        def observation(pid):
            nonlocal calls
            calls += 1
            return self.observed | {'argv':[]} if calls == 1 else self.observed | {'start_ticks':'different'}
        with self.assertRaisesRegex(RuntimeError,'lifetime changed'):
            self.launch(observation)
        self.assertIn('pending_launch',json.loads(self.manifest.read_text()))
        self.assertEqual(json.loads(self.manifest.read_text())['records'],[])


class ManagedStartFence(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='chronolog-start-fence-')
        self.addCleanup(self.directory.cleanup)
        self.work = Path(self.directory.name)
        (self.work/'conf').mkdir()
        self.config = self.work/'conf/service.json'
        self.config.write_text('original-config')
        self.manifest = self.work/'monitor/lifecycle/deployment.json'
        self.calls = []

    def worker(self, host, work, action, deadline, record=None):
        self.calls.append((host, action))
        return {'host':'host-A','records':[]}

    def invoke(self, operation='start', worker=None):
        with contextlib.redirect_stdout(io.StringIO()):
            return lifecycle.fenced_command(self.work, ['host-A'], 500,
                operation, ['/bin/true'], worker=worker or self.worker)

    def test_stopping_or_inflight_start_never_runs_config_mutation(self):
        for phase in ('starting','stopping'):
            with self.subTest(phase=phase):
                lifecycle.atomic_json(self.manifest, {'id':'old','state':phase,'records':[]})
                with mock.patch.object(lifecycle.subprocess, 'Popen') as launch:
                    self.assertEqual(self.invoke(), 27)
                    launch.assert_not_called()
                self.assertEqual(self.config.read_text(), 'original-config')
                self.assertEqual(json.loads(self.manifest.read_text())['id'], 'old')

    def test_failed_host_observation_never_runs_config_mutation(self):
        def unknown(*args):
            raise RuntimeError('SSH failed')
        with mock.patch.object(lifecycle.subprocess, 'Popen') as launch:
            self.assertEqual(self.invoke(worker=unknown), 27)
            launch.assert_not_called()
        self.assertEqual(self.config.read_text(), 'original-config')
        self.assertFalse(self.manifest.exists())

    def test_exclusive_fence_is_held_through_mutation_command(self):
        def launched(*args, **kwargs):
            import fcntl
            lock = (self.work/'monitor/lifecycle/control.lock').open('a+')
            try:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finally:
                lock.close()
            self.assertEqual(kwargs['env']['CHRONOLOG_DEPLOYMENT_ROOT'], str(self.work))
            self.assertEqual(json.loads(self.manifest.read_text())['state'], 'starting')
            return mock.Mock(wait=lambda timeout: 0)
        with mock.patch.object(lifecycle.subprocess, 'Popen', side_effect=launched):
            self.assertEqual(self.invoke(), 0)
        self.assertEqual(self.calls, [('host-A','inventory')])
        self.assertEqual(json.loads(self.manifest.read_text())['state'], 'running')

    def test_occupied_record_prevents_clean_as_well_as_start(self):
        def occupied(*args):
            return {'host':'host-A','records':[{'state':'unknown'}]}
        with mock.patch.object(lifecycle.subprocess, 'Popen') as launch:
            self.assertEqual(self.invoke('clean', occupied), 27)
            launch.assert_not_called()
        self.assertEqual(self.config.read_text(), 'original-config')


    def test_actual_child_inherits_and_validates_the_same_fence(self):
        command = [sys.executable, str(MODULE), '--work-dir', str(self.work), 'guard']
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lifecycle.fenced_command(self.work, ['host-A'], 1000,
                'clean', command, worker=self.worker), 0)
        self.assertEqual(json.loads(self.manifest.read_text())['state'], 'empty')

    def test_local_shell_refuses_incomplete_start_before_config_rewrite(self):
        (self.work/'bin').mkdir()
        template = self.work/'conf/default-chrono-conf.json'
        template.write_text('unchanged-template')
        lifecycle.atomic_json(self.manifest, {'id':'old','state':'stopping','records':[]})
        result = subprocess.run(['bash', str(MODULE.parent/'deploy_local.sh'),
                                 '-d','-w',str(self.work)], capture_output=True, text=True, timeout=2)
        self.assertEqual(result.returncode, 27, result.stdout+result.stderr)
        self.assertEqual(template.read_text(), 'unchanged-template')
        self.assertEqual(json.loads(self.manifest.read_text())['id'], 'old')

    def test_local_shell_stop_without_identity_returns_incomplete(self):
        (self.work/'bin').mkdir()
        result = subprocess.run(['bash', str(MODULE.parent/'deploy_local.sh'),
                                 '-s','-w',str(self.work)], capture_output=True, text=True, timeout=2)
        self.assertEqual(result.returncode, 27, result.stdout+result.stderr)
        self.assertIn('"status":-27', result.stdout)


    def test_cluster_shell_keeps_config_when_prior_stop_is_incomplete(self):
        for role in ('visor','grapher','keeper','player'):
            (self.work/'conf'/('hosts_'+role)).write_text('localhost\n')
        template = self.work/'conf/default-chrono-conf.json'
        template.write_text('unchanged-cluster-template')
        lifecycle.atomic_json(self.manifest, {'id':'old','state':'stopping','records':[]})
        result = subprocess.run(['bash', str(MODULE.parent/'deploy_cluster.sh'),
                                 '-d','-w',str(self.work)], capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 27, result.stdout+result.stderr)
        self.assertEqual(template.read_text(), 'unchanged-cluster-template')
        self.assertFalse((self.work/'conf/hosts_keeper.1').exists())
        self.assertEqual(json.loads(self.manifest.read_text())['id'], 'old')

    def test_remote_launch_intent_is_retained_before_unknown_ssh_dispatch(self):
        lifecycle.atomic_json(self.manifest,{'id':'start-A','state':'starting','records':[],'keepers':[]})
        observed = []
        def unknown(host,work,action,deadline,specification):
            marker = json.loads(self.manifest.read_text()).get('pending_launch')
            self.assertIsNotNone(marker)
            self.assertEqual(marker['specification'],specification)
            observed.append(specification['launch_id'])
            raise TimeoutError('SSH outcome is unknown')
        with mock.patch.dict(lifecycle.os.environ,{'CHRONOLOG_DEPLOYMENT_ID':'start-A'}), \
             mock.patch.object(lifecycle,'validate_guard'), mock.patch.object(lifecycle,'call_worker',side_effect=unknown):
            with self.assertRaises(TimeoutError):
                lifecycle.launch_remote(self.work,'node-B','/bin/service',str(self.config),'keeper','/tmp/log',100)
        self.assertEqual(len(observed),1)
        self.assertEqual(json.loads(self.manifest.read_text())['pending_launch']['specification']['launch_id'],observed[0])


class ManagedStopController(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='chronolog-controller-')
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        (self.work / 'conf').mkdir()
        (self.work / 'monitor/lifecycle').mkdir(parents=True)
        self.clock = 0.0
        self.calls = []
        self.park_unknown = False
        self.ssh_error = False
        self.ready = True
        self.public_ready = True
        self.public_changes = {}
        self.records = []
        for n, role in enumerate(('keeper', 'grapher', 'player', 'visor'), 1):
            config = self.work / 'conf' / (role + '.json')
            body = {'chrono_' + role: {'RecordingGroup': 1, ({'keeper':'KeeperDataStoreAdminService','visor':'VisorKeeperRegistryService','player':'PlayerStoreAdminService'}.get(role, 'DataStoreAdminService')): {'rpc': {
                'protocol_conf': 'ofi+sockets', 'service_ip': '127.0.0.1',
                'service_base_port': 7000 + n, 'service_provider_id': n}}}}
            if role == 'player':
                body['chrono_player']['PlaybackQueryService'] = {'rpc':dict(body['chrono_player']['PlayerStoreAdminService']['rpc'])}
            config.write_text(json.dumps(body))
            record = dict(host='test-host', component=role, config=str(config),
                          config_bytes=config.read_text(), pid=100 + n, uid=1000,
                          executable='/test/chrono-' + role, argv=['/test/chrono-' + role, '--config', str(config)],
                          start_ticks=str(n), boot_id='test-boot', deployment_id='deployment-A',
                          status_file=str(config) + '.lifecycle.json', group=1)
            self.records.append(record)
            lifecycle.atomic_json(lifecycle.record_path(self.work, record['host'], record['config']), record)
            lifecycle.atomic_json(record['status_file'], record | {'state': 'Running', 'attempt': 0, 'status': 0,
                'native_proof':{'generation':1,'initialized_mids':2 if role in ('grapher','visor') else 3,'finalized_mids':0}})
        self.keeper, self.grapher, self.player, self.visor = self.records
        self.target = 'ofi+sockets://127.0.0.1:7001@1'
        self.ready_bytes = json.dumps(self.keeper | dict(state='Ready', status=0, native_generation=1,
            drain_id='same-native-drain', target=self.target, group=1, write_admission_closed=True,
            admitted_ingest_complete=True, archive_confirmed=True, joins_complete=True, outgoing_settled=True))
        Path(self.keeper['config'] + '.ready.json').write_text(self.ready_bytes)
        lifecycle.atomic_json(self.work / 'monitor/lifecycle/deployment.json', {
            'id': 'deployment-A', 'state': 'running', 'records': self.records, 'keepers': [self.keeper],
            'hosts':['local'], 'inventory_complete':True})
        self.terminated = set()

    def sleep(self, duration):
        self.clock += duration

    def worker(self, host, work, action, deadline, record=None):
        self.assertEqual(deadline, 30.0)
        self.calls.append((action, record['component'] if record else 'all'))
        if action == 'inventory':
            return {'records': [r | {'state': 'alive'} for r in self.records]}
        if self.ssh_error:
            raise RuntimeError('SSH observation failed')
        role = record['component']
        if action == 'term':
            if role == 'keeper' and ('visor' not in self.terminated or not Path(self.visor['config']+'.public-quiesced.json').exists()):
                raise RuntimeError('Keeper TERM preceded actual public quiescence')
            if role in ('grapher', 'player'):
                self.assertTrue(Path(self.keeper['config'] + '.retirement-permit.json').exists())
            if role == 'player':
                self.assertTrue(Path(record['config']+'.ingestion-stop-permit.json').exists())
            if role == 'grapher':
                self.assertTrue(Path(record['config'] + '.stop-evidence.json').exists())
            self.terminated.add(role)
        state = 'alive'
        result = {}
        request = Path(self.player['config']+'.queries-quiesce.json')
        if role == 'player' and request.exists() and not getattr(self,'player_queries_unknown',False):
            proof = record | dict(state='QueriesQuiesced', status=0, native_generation=1,
                drain_id='same-query-drain', stop_attempt=json.loads(request.read_text())['stop_attempt'], target='ofi+sockets://127.0.0.1:7003@3',
                query_admission_closed=True, query_native_work_settled=True, keeper_dispatch_closed=True)
            proof.update(getattr(self,'player_query_changes',{}))
            result['queries_quiesced_bytes'] = json.dumps(proof)
            Path(record['config']+'.queries-quiesced.json').write_text(result['queries_quiesced_bytes'])
        if role == 'visor' and 'visor' in self.terminated:
            if self.public_ready:
                public = record | dict(state='PublicQuiesced', status=0, native_generation=1,
                    drain_id='same-public-drain', target='ofi+sockets://127.0.0.1:7004@4',
                    public_admission_closed=True, native_work_settled=True, keeper_dispatch_closed=True)
                public.update(self.public_changes)
                result['public_quiesced_bytes'] = json.dumps(public)
                Path(record['config']+'.public-quiesced.json').write_text(result['public_quiesced_bytes'])
            if Path(record['config']+'.retirement-permit.json').exists():
                self.assertEqual(self.terminated, {'visor', 'keeper', 'grapher', 'player'})
                state = 'stopped'
        elif role == 'keeper' and 'keeper' in self.terminated:
            if self.ready:
                result['ready_bytes'] = self.ready_bytes
            if Path(record['config'] + '.retirement-permit.json').exists():
                state = 'stopped'
        elif role == 'grapher':
            request = Path(record['config'] + '.publisher-park.json')
            if request.exists() and not self.park_unknown:
                packet = json.loads(request.read_text())
                ack = packet | {'state': 'Parked', 'status': 0, 'native_generation': 1}
                result['park_ack_bytes'] = json.dumps(ack)
                Path(record['config'] + '.park-ack.json').write_text(result['park_ack_bytes'])
            if role in self.terminated:
                state = 'stopped'
        elif role in self.terminated:
            state = 'stopped'
        if state == 'stopped':
            mids = 2 if role in ('grapher','visor') else 3
            proof = dict(generation=1, complete=True, abt_uninitialized=True, application_handles=0,
                same_primary=True, primary_releases=1, streams_before_release=1,
                rpc_streams_created=mids, rpc_streams_retired=mids)
            for field in ('initialized_mids','finalized_mids','finalize_calls','finalize_returns',
                          'constructor_units_created','constructor_units_retired','pools_created','pools_retired'):
                proof[field] = mids
            terminal = record | {'state':'Stopped','status':0,'attempt':1,'native_proof':proof}
            result.update(authoritative_absent=True, terminal_bytes=json.dumps(terminal))
            Path(record['status_file']).write_text(result['terminal_bytes'])
        return result | {'state': state, 'native_generation': 1}

    def run_stop(self):
        with contextlib.redirect_stdout(io.StringIO()):
            return lifecycle.stop(self.work, ['local'], 30000, worker=self.worker,
                                  now=lambda: self.clock, sleep=self.sleep)

    def test_missing_both_rpc_stream_counters_never_proves_native_release(self):
        self.assertEqual(self.run_stop(),0)
        observed = self.worker('local',self.work,'inspect',30.0,self.player)
        terminal = json.loads(observed['terminal_bytes'])
        del terminal['native_proof']['rpc_streams_created']
        del terminal['native_proof']['rpc_streams_retired']
        observed['terminal_bytes'] = json.dumps(terminal)
        Path(self.player['status_file']).write_text(observed['terminal_bytes'])
        manifest = json.loads((self.work/'monitor/lifecycle/deployment.json').read_text())
        with self.assertRaisesRegex(RuntimeError,'native release is incomplete'):
            lifecycle.retain_terminal(self.work,self.player,observed,manifest)

    def test_every_player_query_proof_precedes_keeper_term(self):
        self.assertEqual(self.run_stop(), 0)
        request = Path(self.player['config']+'.queries-quiesce.json')
        self.assertTrue(request.exists(), 'managed stop omitted original Player query drain')
        self.assertTrue(Path(self.player['config']+'.queries-quiesced.json').exists())

    def test_missing_player_query_proof_withholds_keeper_term(self):
        self.player_queries_unknown = True
        self.assertEqual(self.run_stop(), 27)
        self.assertNotIn(('term','keeper'), self.calls)
        self.assertEqual(self.terminated, {'visor'})

    def test_stale_or_restarted_player_proof_never_authorizes_keeper_term(self):
        for changes in ({'boot_id':'restarted'}, {'native_generation':0},
                        {'config_bytes':'different'}, {'query_native_work_settled':False}):
            with self.subTest(changes=changes):
                self.player_query_changes = changes
                self.calls.clear()
                self.assertEqual(self.run_stop(),27)
                self.assertNotIn(('term','keeper'),self.calls)
                self.assertEqual(self.terminated,{'visor'})

    def test_any_pending_or_incomplete_host_roster_blocks_stop_before_signals(self):
        path = self.work/'monitor/lifecycle/deployment.json'
        original = json.loads(path.read_text())
        for pending in ({}, {'specification':'unknown'}, None):
            with self.subTest(pending=pending):
                lifecycle.atomic_json(path,original | {'pending_launch':pending})
                self.calls.clear()
                self.assertEqual(self.run_stop(),27)
                self.assertFalse(any(action=='term' for action,_ in self.calls))
        lifecycle.atomic_json(path,original | {'hosts':['unknown-node']})
        self.calls.clear()
        self.assertEqual(self.run_stop(),27)
        self.assertFalse(any(action=='term' for action,_ in self.calls))

    def test_keeper_wait_is_used_to_park_publishers_before_native_retirement(self):
        self.assertEqual(self.run_stop(), 0)
        self.assertTrue(Path(self.keeper['config'] + '.retirement-permit.json').exists())
        self.assertEqual(self.terminated, {'keeper', 'grapher', 'player', 'visor'})
        self.assertLess(self.clock, 30.0)

    def test_completed_stop_is_idempotent_with_exact_stopped_roster(self):
        self.assertEqual(self.run_stop(), 0)
        stop_id = json.loads((self.work/'monitor/lifecycle/deployment.json').read_text())['stop_attempt']
        original = self.worker
        def worker(host, work, action, deadline, record=None):
            if action == 'inventory':
                return {'records':[r | {'state':'stopped'} for r in self.records]}
            return original(host, work, action, deadline, record)
        self.clock = 0.0
        self.worker = worker
        self.assertEqual(self.run_stop(), 0)
        self.assertEqual(json.loads((self.work/'monitor/lifecycle/deployment.json').read_text())['stop_attempt'], stop_id)

    def test_unknown_publisher_keeps_keeper_and_all_dependencies_alive(self):
        self.park_unknown = True
        self.assertEqual(self.run_stop(), 27)
        self.assertEqual(self.terminated, {'visor', 'keeper'})
        self.assertFalse(Path(self.keeper['config'] + '.retirement-permit.json').exists())
        self.assertAlmostEqual(self.clock, 30.0)

    def test_missing_data_readiness_does_not_park_needed_reports(self):
        self.ready = False
        self.assertEqual(self.run_stop(), 27)
        self.assertFalse(Path(self.grapher['config'] + '.publisher-park.json').exists())
        self.assertEqual(self.terminated, {'visor', 'keeper'})

    def test_readiness_wrong_config_boot_generation_or_missing_gate_never_parks(self):
        original = self.ready_bytes
        variants = ({'config_bytes': 'changed'}, {'boot_id': 'other-boot'},
                    {'native_generation': 0}, {'admitted_ingest_complete': False},
                    {'archive_confirmed': False}, {'deployment_id': 'another-deployment'})
        for changes in variants:
            with self.subTest(changes=changes):
                self.ready_bytes = json.dumps(json.loads(original) | changes)
                Path(self.keeper['config'] + '.ready.json').write_text(self.ready_bytes)
                self.assertEqual(self.run_stop(), 27)
                self.assertFalse(Path(self.grapher['config'] + '.publisher-park.json').exists())
                self.assertFalse(Path(self.keeper['config'] + '.retirement-permit.json').exists())

    def test_later_caller_reuses_same_drain_and_stop_identity(self):
        self.park_unknown = True
        self.assertEqual(self.run_stop(), 27)
        manifest_path = self.work / 'monitor/lifecycle/deployment.json'
        stop_id = json.loads(manifest_path.read_text())['stop_attempt']
        self.assertTrue(Path(self.grapher['config'] + '.publisher-park.json').exists())
        first_packet = json.loads(Path(self.grapher['config'] + '.publisher-park.json').read_text())
        self.clock = 0.0
        self.park_unknown = False
        self.assertEqual(self.run_stop(), 0)
        self.assertEqual(json.loads(manifest_path.read_text())['stop_attempt'], stop_id)
        permit = json.loads(Path(self.keeper['config'] + '.retirement-permit.json').read_text())
        self.assertEqual(permit['ready_bytes'], self.ready_bytes)
        self.assertEqual(first_packet['keepers'][0]['ready_bytes'], permit['ready_bytes'])

    def test_public_quiescence_precedes_every_keeper_term(self):
        self.public_ready = False
        self.assertEqual(self.run_stop(), 27)
        self.assertEqual(self.terminated, {'visor'})
        self.assertFalse(Path(self.grapher['config']+'.publisher-park.json').exists())
        self.assertAlmostEqual(self.clock, 30.0)

    def test_stale_visor_public_identity_never_authorizes_keeper_stop(self):
        self.public_changes = {'boot_id':'restarted-visor'}
        self.assertEqual(self.run_stop(), 27)
        self.assertEqual(self.terminated, {'visor'})
        self.assertFalse(Path(self.keeper['config']+'.retirement-permit.json').exists())

    def test_failed_remote_observation_never_becomes_terminal_evidence(self):
        self.ssh_error = True
        self.assertEqual(self.run_stop(), 27)
        self.assertFalse(Path(self.grapher['config'] + '.stop-evidence.json').exists())


class FailedStartTerminal(unittest.TestCase):
    """Synthetic file/controller bindings, not native cleanup evidence."""
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='chronolog-partial-terminal-')
        self.addCleanup(self.temporary.cleanup)
        self.work=Path(self.temporary.name)
        (self.work/'conf').mkdir()
        config=self.work/'conf/player.json'
        config.write_text(json.dumps({'chrono_player':{'RecordingGroup':1}}))
        self.record=dict(host='host-A',pid=123,uid=1000,executable='/test/chrono-player',
            argv=['/test/chrono-player','--config',str(config)],config=str(config),
            config_bytes=config.read_text(),status_file=str(config)+'.lifecycle.json',
            component='player',start_ticks='7',boot_id='boot-A',deployment_id='partial-A')
        self.manifest=dict(id='partial-A',state='stopping',stop_attempt='stop-A',
            records=[self.record],keepers=[],hosts=['local'],stop_inventory_routes=['local'],
            inventory_complete=True,stop_inventory_complete=True,keeper_launches_closed=True)
        lifecycle.atomic_json(self.work/'monitor/lifecycle/deployment.json',self.manifest)
        self.lock=(self.work/'monitor/lifecycle/control.lock').open('a+')
        lifecycle.fcntl.flock(self.lock,lifecycle.fcntl.LOCK_EX|lifecycle.fcntl.LOCK_NB)
        self.addCleanup(self.lock.close)
        native=dict(generation=7,complete=True,abt_uninitialized=True,same_primary=True,
            application_handles=0,primary_releases=1,streams_before_release=1,
            rpc_streams_created=2,rpc_streams_retired=2)
        for key in ('initialized_mids','finalized_mids','finalize_calls','finalize_returns',
                    'constructor_units_created','constructor_units_retired','pools_created','pools_retired'):
            native[key]=2
        self.terminal=self.record|dict(state='Stopped',status=0,startup_succeeded=False,
            startup_interrupted=True,drain_id='drain:boot-A:123:7:7',native_proof=native)
        self.permit=dict(deployment_id='partial-A',stop_attempt='stop-A',recipient=self.record,
            recipient_config_bytes=config.read_text(),recipient_native_generation=7,
            recipient_drain_id=self.terminal['drain_id'],services=[],empty_keeper_roster=True,
            keeper_launches_closed=True)
        self.permit_path=Path(str(config)+'.ingestion-stop-permit.json')
        self.save()

    def save(self):
        Path(self.record['status_file']).write_text(json.dumps(self.terminal))
        self.permit_path.write_text(json.dumps(self.permit))
        lifecycle.atomic_json(self.work/'monitor/lifecycle/deployment.json',self.manifest)

    def retain(self):
        observation=dict(state='stopped',authoritative_absent=True,
            terminal_bytes=Path(self.record['status_file']).read_text())
        return lifecycle.retain_terminal(self.work,self.record,observation,self.manifest)

    def test_exact_interrupted_two_mid_terminal_retains_original_permit_bytes(self):
        result=self.retain()
        self.assertEqual(result['failed_start']['permit_path'],str(self.permit_path))
        self.assertEqual(result['failed_start']['permit_bytes'],self.permit_path.read_text())
        self.assertEqual(json.loads(result['terminal_bytes'])['native_proof']['initialized_mids'],2)
        for key in ('initialized_mids','finalized_mids','finalize_calls','finalize_returns',
                    'constructor_units_created','constructor_units_retired','pools_created','pools_retired',
                    'rpc_streams_created','rpc_streams_retired'):
            self.terminal['native_proof'][key]=1
        self.save()
        self.assertIn('failed_start',self.retain())

    def test_missing_or_successful_start_marker_never_uses_partial_exception(self):
        for field,value in (('startup_succeeded',True),('startup_succeeded',None),
                            ('startup_interrupted',False),('startup_interrupted',None)):
            with self.subTest(field=field,value=value):
                original=self.terminal.copy();self.terminal[field]=value;self.save()
                with self.assertRaises(RuntimeError):self.retain()
                self.terminal=original

    def test_unknown_cleanup_or_constructor_never_becomes_terminal(self):
        for field,value in (('complete',False),('abt_uninitialized',False),('application_handles',1),
                            ('constructor_units_retired',1),('constructor_units_created',3),
                            ('finalize_returns',1),('rpc_streams_retired',1),('same_primary',False)):
            with self.subTest(field=field):
                old=self.terminal['native_proof'][field];self.terminal['native_proof'][field]=value;self.save()
                with self.assertRaises(RuntimeError):self.retain()
                self.terminal['native_proof'][field]=old

    def test_stale_process_config_drain_generation_or_stop_permit_is_refused(self):
        for field,value in (('recipient_config_bytes','wrong'),('recipient_native_generation',8),
                            ('recipient_drain_id','other-drain'),('stop_attempt','other-stop'),
                            ('deployment_id','other-deployment'),('empty_keeper_roster',False)):
            with self.subTest(field=field):
                old=self.permit[field];self.permit[field]=value;self.save()
                with self.assertRaises(RuntimeError):self.retain()
                self.permit[field]=old
        for field,value in (('pid',124),('boot_id','restarted'),('start_ticks','8')):
            with self.subTest(identity=field):
                self.permit['recipient']=self.record|{field:value};self.save()
                with self.assertRaises(RuntimeError):self.retain()
        self.permit['recipient']=self.record

    def test_missing_inventory_pending_launch_or_new_keeper_refuses_partial_proof(self):
        for field,value in (('inventory_complete',False),('stop_inventory_complete',False),
                            ('keeper_launches_closed',False),('hosts',[]),
                            ('stop_inventory_routes',['unverified']),('pending_launch',None),('records',None),
                            ('keepers',[self.record|{'component':'keeper'}])):
            with self.subTest(field=field):
                old=self.manifest.copy();self.manifest[field]=value;self.save()
                try:
                    with self.assertRaises(RuntimeError):self.retain()
                finally:
                    self.manifest=old
        self.manifest['records'].append(self.record|{'component':'keeper','config':'another'})
        self.save()
        with self.assertRaises(RuntimeError):self.retain()

    def test_unrecorded_player_never_uses_empty_roster_exception(self):
        self.manifest['records']=[]
        self.save()
        with self.assertRaises(RuntimeError):self.retain()

    def test_malformed_permit_or_recipient_is_explicitly_incomplete(self):
        for packet in ([],self.permit|{'recipient':None}):
            with self.subTest(packet=packet):
                self.permit_path.write_text(json.dumps(packet))
                with self.assertRaises(RuntimeError):self.retain()

    def test_partial_proof_requires_actual_closed_start_fence(self):
        lifecycle.fcntl.flock(self.lock,lifecycle.fcntl.LOCK_UN)
        with self.assertRaises(RuntimeError):self.retain()

    def test_normal_three_mid_proof_is_not_reduced_to_two(self):
        self.terminal['startup_succeeded']=True
        self.save()
        with self.assertRaises(RuntimeError):self.retain()
        for key in ('initialized_mids','finalized_mids','finalize_calls','finalize_returns',
                    'constructor_units_created','constructor_units_retired','pools_created','pools_retired'):
            self.terminal['native_proof'][key]=3
        self.save()
        self.assertNotIn('failed_start',self.retain())

    def test_ingestion_permit_is_immutable_across_later_caller_attempts(self):
        live=self.record|{'state':'alive'}
        self.terminal['state']='Draining';self.terminal['status']=-27
        Path(self.record['status_file']).write_text(json.dumps(self.terminal))
        self.permit_path.unlink()
        lifecycle.terminal_evidence(self.work,[live],{},self.manifest)
        original=self.permit_path.read_bytes()
        self.terminal['attempt']=9;self.terminal['elapsed_ms']=999
        Path(self.record['status_file']).write_text(json.dumps(self.terminal))
        lifecycle.terminal_evidence(self.work,[live],{},self.manifest)
        self.assertEqual(original,self.permit_path.read_bytes())
        self.terminal['drain_id']='another-drain'
        Path(self.record['status_file']).write_text(json.dumps(self.terminal))
        with self.assertRaises(RuntimeError):
            lifecycle.terminal_evidence(self.work,[live],{},self.manifest)
        self.assertEqual(original,self.permit_path.read_bytes())


class ShippedConfigContract(unittest.TestCase):
    def test_keeper_endpoint_is_read_from_the_actual_shipped_config(self):
        config = MODULE.parents[2] / 'conf/default_conf.json.in'
        record = {'component': 'keeper', 'config': str(config)}
        # Independently fixed shipped default, not another generated fixture.
        self.assertEqual(lifecycle.target(record), 'ofi+sockets://127.0.0.1:7777@77')
        self.assertEqual(lifecycle.group(record), 7)
        self.assertEqual(lifecycle.visor_target({'component':'visor','config':str(config)}),
                         'ofi+sockets://127.0.0.1:8888@88')
        self.assertEqual(lifecycle.player_target({'component':'player','config':str(config)}),
                         'ofi+sockets://127.0.0.1:2525@25')


class KeeperRestart(unittest.TestCase):
    """restart-keeper replaces the record of a keeper whose process is gone."""
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='chronolog-keeper-restart-')
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name)
        (self.work/'conf').mkdir()
        self.config = self.work/'conf/chrono-keeper-conf-1.json'
        self.config.write_text('{}')
        self.keeper = dict(host='host-A', pid=123, uid=1000, executable='/test/chrono-keeper',
                           config=str(self.config.resolve()), component='keeper',
                           start_ticks='7', boot_id='boot-A', deployment_id='run-A')
        self.visor = self.keeper | dict(pid=100, executable='/test/chrono-visor', component='visor',
                                        config=str(self.work/'conf/chrono-visor-conf.json'))
        self.manifest = self.work/'monitor/lifecycle/deployment.json'
        self.save(state='running')
        self.calls = []
        self.keeper_state = 'unknown'

    def save(self, **fields):
        lifecycle.atomic_json(self.manifest, dict(id='run-A', records=[self.visor, self.keeper],
                                                  keepers=[self.keeper], hosts=['local']) | fields)

    def worker(self, host, work, action, deadline, record=None):
        self.calls.append(action)
        if action == 'inventory':
            return {'host':'host-A', 'records':[self.visor | {'state':'alive'},
                                                self.keeper | {'state':self.keeper_state}]}
        # launch: what record_process and launch do on the host
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(manifest['state'], 'starting')
        self.assertEqual(manifest['pending_launch']['specification'], record)
        self.launched = record
        replacement = self.keeper | dict(pid=456, start_ticks='9')
        manifest.pop('pending_launch')
        manifest['records'].append(replacement)
        manifest['keepers'].append(replacement)
        lifecycle.atomic_json(self.manifest, manifest)
        return replacement

    def restart(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = lifecycle.restart_keeper(self.work, 'local', 1000, '/test/chrono-keeper',
                                              str(self.config), str(self.work/'monitor/keeper-1.launch.log'),
                                              worker=self.worker)
        return result, output.getvalue()

    def test_keeper_killed_outside_the_stop_is_replaced_by_its_relaunch(self):
        result, output = self.restart()
        self.assertEqual(result, 0, output)
        self.assertEqual(self.calls, ['inventory', 'launch'])
        self.assertEqual(self.launched['config'], str(self.config.resolve()))
        self.assertTrue(self.launched['append_output'])
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(manifest['state'], 'running')
        self.assertEqual([r['pid'] for r in manifest['records']], [100, 456])
        self.assertEqual([r['pid'] for r in manifest['keepers']], [456])

    def test_running_keeper_is_never_relaunched(self):
        self.keeper_state = 'alive'
        result, output = self.restart()
        self.assertEqual(result, 27)
        self.assertIn('still running', output)
        self.assertEqual(self.calls, ['inventory'])
        manifest = json.loads(self.manifest.read_text())
        self.assertEqual(manifest['state'], 'running')
        self.assertEqual([r['pid'] for r in manifest['records']], [100, 123])

    def test_restart_is_refused_outside_a_running_deployment(self):
        for fields in (dict(state='stopping', keeper_launches_closed=True), dict(state='starting'),
                       dict(state='running', pending_launch={'route':'local'})):
            with self.subTest(fields=fields):
                self.save(**fields)
                self.calls.clear()
                result, output = self.restart()
                self.assertEqual(result, 27)
                self.assertEqual(self.calls, [])
                self.assertEqual(json.loads(self.manifest.read_text())['state'], fields['state'])

    def test_unknown_launch_outcome_keeps_the_pending_launch(self):
        def unknown(host, work, action, deadline, record=None):
            if action == 'inventory':
                return self.worker(host, work, action, deadline, record)
            raise TimeoutError('SSH outcome is unknown')
        with contextlib.redirect_stdout(io.StringIO()):
            result = lifecycle.restart_keeper(self.work, 'local', 1000, '/test/chrono-keeper', str(self.config),
                                              str(self.work/'monitor/keeper-1.launch.log'), worker=unknown)
        self.assertEqual(result, 27)
        manifest = json.loads(self.manifest.read_text())
        # The managed stop refuses while the launch is pending, as after a start.
        self.assertEqual(manifest['state'], 'starting')
        self.assertIn('pending_launch', manifest)


if __name__ == '__main__':
    unittest.main()
