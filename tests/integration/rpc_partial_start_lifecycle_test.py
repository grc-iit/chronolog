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
from rpc_lifecycle_deadline_test import fixture_processes,signal_owned,dispose_fixture,patch


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
            if before.get('native_proof',{}).get('initialized_mids')!=2 or before['native_proof']['finalized_mids']!=0:
                raise RuntimeError('partial fixture did not construct exactly two original MIDs')
            starter.wait(timeout=35)
        (logs/'start-return.json').write_text(json.dumps({'returncode':starter.returncode}))
        if starter.returncode!=27:
            raise RuntimeError('interrupted managed start did not preserve incomplete status')
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
        print(json.dumps({'case':'known_partial_player_cleanup','protocol':args.protocol,'trace':args.trace,
            'status':'SCOPED_PASS','player_raw_wait':exits[player],'player_native':native,
            'aggregate_return':controller.returncode,'full_deployment_retirement_proved':False}),flush=True)
    finally:
        (logs/'constructor.release').touch()
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
