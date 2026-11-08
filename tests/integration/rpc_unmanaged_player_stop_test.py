#!/usr/bin/env python3
"""Unmanaged TERM retains Player ingestion for a live dual-source Keeper.

Owned fixture disposal is not native retirement. Shipped policies stay intact.
"""
import argparse
import copy
import ctypes
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
from rpc_lifecycle_deadline_test import fixture_processes,identity,signal_owned,dispose_fixture,patch,result_line


def wait(predicate,seconds,reason):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        value=predicate()
        if value:return value
        time.sleep(.01)
    raise RuntimeError(reason)


def run(args):
    installed=Path(os.environ['WORK_DIR']).resolve()
    build=Path(os.environ['BUILD_DIR']).resolve()
    logs=Path(args.logs).resolve();logs.mkdir(parents=True,exist_ok=False)
    shutil.copy2(__file__,logs/'executed-fixture.py')
    work=logs/'runtime-work';(work/'conf').mkdir(parents=True)
    for name in ('bin','lib','tools'):(work/name).symlink_to(installed/name,target_is_directory=True)
    if fixture_processes(work):raise RuntimeError('host fixture executables occupied')
    if ctypes.CDLL(None).prctl(36,1,0,0,0)!=0:raise RuntimeError('cannot adopt fixture descendants')
    configuration=json.loads((installed/'conf/default-chrono-conf.json').read_text())
    patch(configuration,args.protocol,False,True)
    for role in ('visor','grapher','player','keeper'):
        policy=configuration['chrono_'+role]['RpcPolicy']
        if (policy['attempt_ms'],policy['startup_ms'],policy['shutdown_ms'])!=(5000,30000,30000):
            raise RuntimeError('requires actual shipped lifecycle defaults')
    for role in ('visor','grapher','player','keeper'):
        configuration['chrono_'+role]['Monitoring']['monitor']['file']=str(work/'monitor'/('chrono-'+role+'.log'))
    (work/'monitor').mkdir();(work/'output').mkdir()
    configuration['chrono_grapher']['ExtractionModule']['extractors']['hdf5_archive_extractor']['hdf5_archive_dir']=str(work/'output')
    configuration['chrono_player']['ArchiveReaders']['story_files_dir']=str(work/'output')
    for configured in configuration['chrono_keeper']['ExtractionModule']['extractors'].values():
        if configured['type']=='single_endpoint_rdma_extractor':
            configured['type']='dual_endpoint_rdma_extractor'
            configured['grapher_receiving_endpoint']=configured.pop('receiving_endpoint')
            configured['player_receiving_endpoint']=copy.deepcopy(configuration['chrono_player']['PlayerRecordingService']['rpc'])
    env=dict(os.environ)
    for key in ('CHRONOLOG_DEPLOYMENT_ID','CHRONOLOG_DEPLOYMENT_ROOT','CHRONOLOG_DEPLOYMENT_GUARD'):env.pop(key,None)
    if args.protocol=='ofi+tcp':env['FI_TCP_IFACE']='lo'
    if args.stats_stop or args.stats_dead:
        env['LD_PRELOAD']=str(build/'tests/integration/watermark-replay/librpc_lifecycle_native_spy.so')
        env['CHRONOLOG_TEST_NATIVE_TRACE']=str(logs/'native-trace.jsonl')
    services={};owned={};probe=None;probe_identity=None;probe_log=None
    try:
        for role in ('visor','grapher','player','keeper'):
            config=work/'conf'/('chrono-'+role+'-conf.json')
            config.write_text(json.dumps(configuration,indent=2))
            with (work/'monitor'/('chrono-'+role+'.launch.log')).open('w') as launch:
                child=subprocess.Popen([str(work/'bin'/('chrono-'+role)),'--config',str(config)],env=env,
                    cwd=work,stdout=launch,stderr=subprocess.STDOUT)
            services[role]=(child,config)
            status=Path(str(config)+'.lifecycle.json')
            def running():
                if child.poll() is not None:raise RuntimeError(role+' exited during startup')
                return status.exists() and json.loads(status.read_text()).get('state')=='Running'
            wait(running,32,role+' did not reach Running under shipped startup budget')
            observed=identity(child.pid)
            if not observed:raise RuntimeError('original service identity unavailable')
            owned[child.pid]=observed
        child,config=services['player'];status=Path(str(config)+'.lifecycle.json')
        stats_entries=[]
        if args.stats_stop or args.stats_dead:
            visor=services['visor'][0]
            paused_ns=time.monotonic_ns()
            if args.stats_dead:
                signal_owned(visor.pid,owned[visor.pid],signal.SIGKILL)
                waited,raw=os.waitpid(visor.pid,0);visor.returncode=os.waitstatus_to_exitcode(raw)
                (logs/'stats-peer-raw-wait.json').write_text(json.dumps({'pid':waited,'raw_wait':raw,'exit_code':visor.returncode},indent=2))
                if waited!=visor.pid or visor.returncode!=-signal.SIGKILL:raise RuntimeError('dead stats peer not observed')
            else:signal_owned(visor.pid,owned[visor.pid],signal.SIGSTOP)
            def stats_started():
                trace=logs/'native-trace.jsonl'
                rows=[json.loads(row) for row in trace.read_text().splitlines()] if trace.exists() else []
                return [row for row in rows if row['pid']==child.pid and row['name']=='handle_player_stats_msg'
                    and row['phase']=='forward_enter' and row['ns']>paused_ns]
            stats_entries=wait(stats_started,15,'actual Player stats did not run while real Visor unavailable')
        signal_owned(child.pid,owned[child.pid],signal.SIGTERM)
        entered=time.monotonic()
        wait(lambda:child.poll() is not None or json.loads(status.read_text()).get('state') in ('Draining','Stopped'),8,
             'direct Player TERM did not publish its state')
        state=json.loads(status.read_text());proof=state.get('native_proof',{})
        (logs/'unmanaged-player-after-term.json').write_text(json.dumps({'status':state,'exit_code':child.poll(),
            'observed_ms':int((time.monotonic()-entered)*1000)},indent=2))
        if (child.poll() is not None or state.get('state')!='Draining' or state.get('status')!=-27
            or state.get('deployment_id')!='' or proof.get('initialized_mids')!=3 or proof.get('finalized_mids')!=0
            or state.get('native_proof',{}).get('application_handles',0)<=0):
            raise RuntimeError('unmanaged Player retired or closed required dependencies without proof')
        if args.stats_stop or args.stats_dead:
            observed_ms=int((time.monotonic()-entered)*1000)
            if observed_ms>6500:raise RuntimeError('TERM was blocked by a stats attempt')
            # A later real peer death must not turn local stats send into remote ACK,
            # or let repeated TERM retire the dependency-retaining Player.
            if not args.stats_dead:
                signal_owned(visor.pid,owned[visor.pid],signal.SIGKILL)
                waited,raw=os.waitpid(visor.pid,0)
                visor.returncode=os.waitstatus_to_exitcode(raw)
                (logs/'stats-peer-raw-wait.json').write_text(json.dumps({'pid':waited,'raw_wait':raw,'exit_code':visor.returncode},indent=2))
                if waited!=visor.pid or visor.returncode!=-signal.SIGKILL:raise RuntimeError('owned stopped Visor death not observed')
            signal_owned(child.pid,owned[child.pid],signal.SIGTERM)
            def repeated_incomplete():
                current=json.loads(status.read_text())
                return (current.get('attempt',0)>state.get('attempt',0) and current.get('state')=='Draining'
                    and current.get('status')==-27 and current)
            dead_state=wait(repeated_incomplete,8,'repeated TERM after peer death did not finish its incomplete attempt')
            (logs/'unmanaged-player-after-visor-death.json').write_text(json.dumps(dead_state,indent=2))
            if (child.poll() is not None or identity(child.pid)!=owned[child.pid]
                or dead_state.get('state')!='Draining' or dead_state.get('status')!=-27
                or dead_state['native_proof']['finalized_mids']!=0):
                raise RuntimeError('peer death/repeated TERM retired original Player')
            if args.stats_dead:
                # Observe real retention after the original attempt expires;
                # elapsed time itself never establishes settlement.
                boundary=stats_entries[0]['ns']+5100000000
                wait(lambda:time.monotonic_ns()>=boundary,6,'original stats attempt boundary not reached')
                retained=json.loads(status.read_text())
                (logs/'stats-after-original-5s.json').write_text(json.dumps(retained,indent=2))
                if (identity(child.pid)!=owned[child.pid] or child.poll() is not None
                    or retained.get('state')!='Draining' or retained.get('status')!=-27
                    or retained.get('drain_id')!=state.get('drain_id')
                    or retained['native_proof']['initialized_mids']!=3
                    or retained['native_proof']['finalized_mids']!=0):
                    raise RuntimeError('stats attempt expiry lost original Player generation/dependencies')
            print(json.dumps({'case':'actual_player_stats_term_dead_visor' if args.stats_dead else 'actual_player_stats_term_paused_then_dead_visor','status':'PASS',
                'protocol':args.protocol,'trace':False,'passive_native_spy':True,'term_observed_ms':observed_ms,
                'actual_stats_entries':stats_entries,'retained_native':proof,
                'stats_local_completion_is_not_peer_ack':True,'dead_visor_exit_code':visor.returncode,
                'same_player_after_peer_death':True,'native_retirement_proved':False}),flush=True)
            return
        # New public writes after query closure still need Player-first dual input.
        client_conf=json.loads((installed/'conf/default-chrono-client-conf.json').read_text())
        patch(client_conf,args.protocol,False,True)
        client=work/'conf/chrono-client-conf.json';client.write_text(json.dumps(client_conf,indent=2))
        probe_log=(logs/'public-writer.log').open('w')
        probe=subprocess.Popen([str(build/'tests/integration/watermark-replay/chronolog-test-rpc-lifecycle-probe'),
            str(client),'--recovery','UnmanagedPlayer-'+str(child.pid)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
            stderr=probe_log,text=True,bufsize=1,env=env)
        probe_identity=wait(lambda:identity(probe.pid),5,'public probe identity unavailable')
        ready=result_line(probe,probe_log,30)
        if not ready.startswith('READY 8 '):raise RuntimeError('public producer did not accept eight events: '+ready)
        probe.stdin.write('QUIT\n');probe.stdin.flush();probe.wait(timeout=35)
        probe_log.write(probe.stdout.read());probe_log.flush()
        if probe.returncode!=0:raise RuntimeError('public producer failed')
        def received(role):
            path=work/'monitor'/('chrono-'+role+'.log')
            text=path.read_text() if path.exists() else ''
            return [line for line in text.splitlines() if 'Received chunk' in line and 'eventCount8' in line]
        p_receipt=wait(lambda:received('player'),25,'Player did not receive dual chunk after TERM')
        g_receipt=wait(lambda:received('grapher'),10,'Grapher did not receive dual chunk after Player')
        if child.poll() is not None or identity(child.pid)!=owned[child.pid]:raise RuntimeError('original Player did not remain alive')
        final=json.loads(status.read_text())
        (logs/'retained-after-dual-input.json').write_text(json.dumps(final,indent=2))
        if final['native_proof']['finalized_mids']!=0:raise RuntimeError('late input lost its original native generation')
        print(json.dumps({'case':'unmanaged_player_direct_term_dual','status':'PASS','protocol':args.protocol,
            'trace':False,'player_pid':child.pid,'same_original_player':True,'retained_mids':3,
            'player_receipt':p_receipt,'grapher_receipt':g_receipt,'native_retirement_proved':False}),flush=True)
    finally:
        if probe and probe.poll() is None:
            if identity(probe.pid)!=probe_identity:raise RuntimeError('public probe identity changed before disposal')
            signal_owned(probe.pid,probe_identity,signal.SIGTERM)
            try:probe.wait(timeout=3)
            except subprocess.TimeoutExpired:
                signal_owned(probe.pid,probe_identity,signal.SIGKILL);probe.wait(timeout=3)
        if probe_log:probe_log.close()
        owned.update(fixture_processes(work))
        dispose_fixture(owned,work,logs)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--protocol',choices=('ofi+sockets','ofi+tcp'),required=True)
    parser.add_argument('--logs',required=True);mode=parser.add_mutually_exclusive_group()
    mode.add_argument('--stats-stop',action='store_true');mode.add_argument('--stats-dead',action='store_true');run(parser.parse_args())
