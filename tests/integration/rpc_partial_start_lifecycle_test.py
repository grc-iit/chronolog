#!/usr/bin/env python3
"""Actual interrupted Player construction. Cleanup disposal is never retirement."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
from rpc_lifecycle_deadline_test import fixture_processes,signal_owned,dispose_fixture,patch,identity


def wait(predicate,seconds,reason):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        result=predicate()
        if result:
            return result
        time.sleep(.01)
    raise RuntimeError(reason)


def run(args):
    installed=Path(os.environ['WORK_DIR']).resolve()
    build=Path(os.environ['BUILD_DIR']).resolve()
    logs=Path(args.logs).resolve()
    logs.mkdir(parents=True,exist_ok=False)
    shutil.copy2(__file__,logs/'executed-fixture.py')
    work=logs/'runtime-work'
    (work/'conf').mkdir(parents=True)
    for name in ('bin','lib','tools'):
        (work/name).symlink_to(installed/name,target_is_directory=True)
    for name in ('default-chrono-conf.json','default-chrono-client-conf.json'):
        value=json.loads((installed/'conf'/name).read_text())
        patch(value,args.protocol,args.trace,False)
        (work/'conf'/name).write_text(json.dumps(value,indent=2))
    if ctypes.CDLL(None,use_errno=True).prctl(36,1,0,0,0)!=0:
        raise RuntimeError('cannot retain original child exit statuses')
    if fixture_processes(work):
        raise RuntimeError('installed executables occupied before partial fixture')
    env=dict(os.environ,LD_PRELOAD=str(build/'tests/integration/watermark-replay/librpc_player_init_hold.so'),
             CHRONOLOG_TEST_PLAYER_INIT_HOLD=str(logs/'constructor'))
    if args.protocol=='ofi+tcp':
        env['FI_TCP_IFACE']='lo'
    owned={}
    starter=None
    controller=None
    entered=None
    exits={}
    try:
        with (logs/'start.log').open('w') as output:
            starter=subprocess.Popen([str(work/'tools/deploy/deploy_local.sh'),'-d','-w',str(work)],
                stdout=output,stderr=subprocess.STDOUT,env=env)
            marker=logs/'constructor.entered'
            wait(marker.exists,20,'actual second Player constructor did not enter hold')
            fields=marker.read_text().split()
            if fields[2:]!=['1','2097152','8']:
                raise RuntimeError('held constructor lacks actual native2MiB/cache8 context: '+repr(fields))
            owned=fixture_processes(work)
            player=int(fields[0])
            if player not in owned or not owned[player][0].endswith('/chrono-player') or len(owned)!=3:
                raise RuntimeError('held partial launch identities are unverified')
            player_config=work/'conf/chrono-player-conf-1.json'
            status=Path(str(player_config)+'.lifecycle.json')
            signal_owned(player,owned[player],signal.SIGTERM)
            wait(lambda: status.exists() and json.loads(status.read_text()).get('state')=='Quiescing',5,
                 'interrupted startup did not retain original construction')
            (logs/'held-construction-status.json').write_text(status.read_text())
            (logs/'constructor.release').touch()
            wait(lambda: (logs/'constructor.completed').exists(),10,'original constructor did not continue')
            if (logs/'constructor.completed').read_text().strip()!='1':
                raise RuntimeError('original constructor failed after release')
            wait(lambda: status.exists() and json.loads(status.read_text()).get('state')=='Draining',10,
                 'known partial Player did not retain dependencies')
            before=json.loads(status.read_text())
            (logs/'known-partial-before-stop.json').write_text(json.dumps(before,indent=2))
            if (before.get('native_proof',{}).get('initialized_mids')!=2 or before['native_proof']['finalized_mids']!=0
                or before.get('startup_succeeded') is not False or before.get('startup_interrupted') is not True
                or not before.get('drain_id')):
                raise RuntimeError('partial fixture lacks explicit interrupted-start/original two-MID proof')
            starter.wait(timeout=35)
        (logs/'start-return.json').write_text(json.dumps({'returncode':starter.returncode}))
        if starter.returncode!=27:
            raise RuntimeError('interrupted managed start did not preserve incomplete status')
        def adopted_children():
            parents={}
            for pid,expected in owned.items():
                if identity(pid)!=expected:
                    raise RuntimeError('original service identity changed before fixture adoption')
                fields=(Path('/proc')/str(pid)/'stat').read_text().rsplit(')',1)[1].split()
                parents[pid]=int(fields[1])
            return parents if all(parent==os.getpid() for parent in parents.values()) else None
        # The original launch worker can finish just after its caller returns.
        # Wait for actual subreaper adoption, never infer a normal wait from
        # ECHILD or race fixture disposal against the active stop controller.
        parents=wait(adopted_children,5,'original launch workers did not release their children')
        (logs/'adopted-original-children.json').write_text(json.dumps({'subreaper_pid':os.getpid(),'parents':parents},indent=2))
        manifest=json.loads((work/'monitor/lifecycle/deployment.json').read_text())
        (logs/'failed-start-manifest.json').write_text(json.dumps(manifest,indent=2))
        if manifest.get('pending_launch') or manifest['keepers'] or len(manifest['records'])!=3:
            raise RuntimeError('empty Keeper closed-start authority is unverified')
        with (logs/'stop.log').open('w') as output:
            controller=subprocess.Popen([str(work/'tools/deploy/deploy_local.sh'),'-s','-w',str(work)],
                stdout=output,stderr=subprocess.STDOUT,env=env)
            entered=time.monotonic()
            while controller.poll() is None:
                for pid in owned:
                    if pid not in exits:
                        found,raw=os.waitpid(pid,os.WNOHANG)
                        if found:
                            exits[pid]=raw
                            (logs/'raw-wait-statuses.json').write_text(json.dumps(exits,indent=2))
                if time.monotonic()-entered>33:
                    controller.terminate()
                    controller.wait(timeout=3)
                    raise RuntimeError('partial stop exceeded whole30s command')
                time.sleep(.005)
        for pid in owned:
            if pid not in exits:
                found,raw=os.waitpid(pid,os.WNOHANG)
                if found: exits[pid]=raw
        (logs/'raw-wait-statuses.json').write_text(json.dumps(exits,indent=2))
        result=json.loads(status.read_text())
        (logs/'partial-final-status.json').write_text(json.dumps(result,indent=2))
        native=result.get('native_proof',{})
        if not (result.get('state')=='Stopped' and result.get('status')==0 and exits.get(player)==256
            and all(native.get(key)==2 for key in ('initialized_mids','finalized_mids','finalize_calls','finalize_returns',
                'constructor_units_created','constructor_units_retired','pools_created','pools_retired'))
            and native.get('complete') is True and native.get('abt_uninitialized') is True
            and native.get('same_primary') is True and native.get('primary_releases')==1
            and native.get('streams_before_release')==1 and native.get('application_handles')==0):
            raise RuntimeError('known partial Player lacks actual original two-MID physical cleanup/raw failed-start exit')
        results=[]
        for record in manifest['records']:
            state=json.loads(Path(record['status_file']).read_text())
            (logs/('final-'+record['component']+'-status.json')).write_text(json.dumps(state,indent=2))
            proof=state.get('native_proof',{})
            expected_wait=256 if record['component']=='player' else 0
            if (exits.get(record['pid'])!=expected_wait or state.get('state')!='Stopped' or state.get('status')!=0
                or not all(proof.get(key)==2 for key in ('initialized_mids','finalized_mids','finalize_calls',
                    'finalize_returns','constructor_units_created','constructor_units_retired','pools_created','pools_retired'))
                or not all(proof.get(key) is True for key in ('complete','abt_uninitialized','same_primary'))
                or proof.get('application_handles')!=0 or proof.get('primary_releases')!=1
                or proof.get('streams_before_release')!=1 or proof.get('rpc_streams_created',0)<=0
                or proof['rpc_streams_created']!=proof.get('rpc_streams_retired')):
                raise RuntimeError('partial deployment lacks actual normal wait/native release: '+record['component'])
            results.append({'component':record['component'],'wait_status':exits[record['pid']],
                'exit_code':os.WEXITSTATUS(exits[record['pid']]),'native_proof':proof})
        if (controller.returncode!=0 or time.monotonic()-entered>33
            or result.get('startup_succeeded') is not False or result.get('startup_interrupted') is not True
            or result.get('drain_id')!=before['drain_id']):
            raise RuntimeError('partial-start stop lost its same-drain/default budget/failed-start identity')
        visor=next(record for record in manifest['records'] if record['component']=='visor')
        final_permit=json.loads(Path(visor['config']+'.retirement-permit.json').read_text())
        retained=next(item for item in final_permit['services'] if item['record']['component']=='player')
        ingestion_path=Path(str(player_config)+'.ingestion-stop-permit.json')
        if (retained['failed_start']['permit_path']!=str(ingestion_path)
            or retained['failed_start']['permit_bytes']!=ingestion_path.read_text()
            or json.loads(ingestion_path.read_text())['recipient_drain_id']!=before['drain_id']):
            raise RuntimeError('final Visor permit did not retain the exact original partial-start permit')
        print(json.dumps({'case':'known_partial_player_cleanup','protocol':args.protocol,'trace':args.trace,
            'status':'PASS','player_raw_wait':exits[player],'player_native':native,'results':results,
            'aggregate_return':controller.returncode,'full_deployment_retirement_proved':True}),flush=True)
    finally:
        (logs/'constructor.release').touch()
        if controller and controller.poll() is None:
            until=(entered if entered is not None else time.monotonic())+33
            while controller.poll() is None and time.monotonic()<until:
                for pid in owned:
                    if pid in exits: continue
                    try:
                        found,raw=os.waitpid(pid,os.WNOHANG)
                    except ChildProcessError:
                        continue  # Still unobserved, never a successful wait.
                    if found:
                        exits[pid]=raw
                        (logs/'raw-wait-statuses.json').write_text(json.dumps(exits,indent=2))
                time.sleep(.005)
            if controller.poll() is None:
                controller.terminate()  # Exact own helper, never a daemon.
                controller.wait(timeout=3)
        if starter and starter.poll() is None:
            try: starter.wait(timeout=35)
            except subprocess.TimeoutExpired:
                starter.kill();starter.wait(timeout=3)
        owned.update(fixture_processes(work))
        snapshots=logs/'before-fixture-cleanup-config'
        snapshots.mkdir(exist_ok=True)
        for path in (work/'conf').glob('chrono-*.json*'):
            if path.is_file(): shutil.copy2(path,snapshots/path.name)
        dispose_fixture(owned,work,logs)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--protocol',choices=('ofi+sockets','ofi+tcp'),required=True)
    parser.add_argument('--logs',required=True)
    parser.add_argument('--trace',action='store_true')
    run(parser.parse_args())
