#!/usr/bin/env python3
"""Default 5s/30s lifecycle evidence against actual chrono-visor processes.

Only newly launched exact fixture identities are signalled. Peer death and
fixture disposal never prove native settlement. No policy is shortened.
"""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import time
from rpc_lifecycle_deadline_test import identity,signal_owned,patch,fixture_processes,dispose_fixture


def wait(predicate,seconds,reason):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        value=predicate()
        if value:return value
        time.sleep(.01)
    raise RuntimeError(reason)


class Fixture:
    def __init__(self,args):
        self.args=args;self.logs=Path(args.logs).resolve();self.logs.mkdir(parents=True,exist_ok=False)
        shutil.copy2(__file__,self.logs/'executed-fixture.py')
        self.installed=Path(os.environ['WORK_DIR']).resolve();self.build=Path(os.environ['BUILD_DIR']).resolve()
        self.work=self.logs/'runtime-work';(self.work/'conf').mkdir(parents=True);(self.work/'monitor').mkdir()
        (self.work/'output').mkdir()
        for name in ('bin','lib','tools'):(self.work/name).symlink_to(self.installed/name,target_is_directory=True)
        if fixture_processes(self.work):raise RuntimeError('native fixture executables already occupied')
        self.configuration=json.loads((self.installed/'conf/default-chrono-conf.json').read_text())
        patch(self.configuration,args.protocol,False,False)
        for role in ('visor','keeper','grapher','player'):
            policy=self.configuration['chrono_'+role]['RpcPolicy']
            if (policy['attempt_ms'],policy['startup_ms'],policy['shutdown_ms'])!=(5000,30000,30000):
                raise RuntimeError('requires actual shipped lifecycle default budgets')
            self.configuration['chrono_'+role]['Monitoring']['monitor']['file']=str(self.work/'monitor'/('chrono-'+role+'.log'))
        self.env=dict(os.environ)
        for key in ('CHRONOLOG_DEPLOYMENT_ID','CHRONOLOG_DEPLOYMENT_ROOT','CHRONOLOG_DEPLOYMENT_GUARD'):self.env.pop(key,None)
        if args.protocol=='ofi+tcp':self.env['FI_TCP_IFACE']='lo'
        self.env['LD_PRELOAD']=str(self.build/'tests/integration/watermark-replay/librpc_lifecycle_native_spy.so')
        self.env['CHRONOLOG_TEST_NATIVE_TRACE']=str(self.logs/'native-trace.jsonl')
        self.children={};self.owned={};self.raw={};self.probe=None;self.probe_identity=None;self.buffer=b'';self.results=[]
        self.probe_output=(self.logs/'probe.stdout').open('w');self.probe_error=(self.logs/'probe.stderr').open('w')

    def service(self,role):
        config=self.work/'conf'/('chrono-'+role+'-conf.json');config.write_text(json.dumps(self.configuration,indent=2))
        with (self.work/'monitor'/('chrono-'+role+'.launch.log')).open('w') as output:
            child=subprocess.Popen([str(self.work/'bin'/('chrono-'+role)),'--config',str(config)],env=self.env,
                cwd=self.work,stdout=output,stderr=subprocess.STDOUT)
        self.children[role]=(child,config);start=time.monotonic()
        expected=wait(lambda:identity(child.pid),5,'service identity unavailable')
        self.owned[child.pid]=expected
        if role=='visor':
            wait(lambda:Path(str(config)+'.lifecycle.json').exists() and self.state(role).get('state')=='Running',32,
                 'actual Visor did not reach Running')
        return child,start

    def state(self,role):
        return json.loads(Path(str(self.children[role][1])+'.lifecycle.json').read_text())

    def peer_signal(self,number):
        child=self.children['visor'][0];signal_owned(child.pid,self.owned[child.pid],number)

    def trace(self,name=None,pid=None,phase='forward_enter'):
        path=self.logs/'native-trace.jsonl'
        raw=path.read_text() if path.exists() else ''
        rows=[json.loads(line) for line in raw.splitlines() if line]
        return [row for row in rows if row['phase']==phase and (name is None or row['name']==name)
                and (pid is None or row['pid']==pid)]

    def launch_probe(self,mode):
        endpoint=self.configuration['chrono_visor']['VisorKeeperRegistryService']['rpc']
        address=endpoint['protocol_conf']+'://'+endpoint['service_ip']+':'+str(endpoint['service_base_port'])
        if endpoint['service_provider_id']!=88:raise RuntimeError('requires shipped registry provider88')
        self.probe=subprocess.Popen([str(self.build/'tests/integration/watermark-replay/chronolog-test-rpc-registry-budget-probe'),
            self.args.protocol,address,mode],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=self.probe_error,
            env=self.env,bufsize=0)
        self.probe_identity=wait(lambda:identity(self.probe.pid),5,'probe identity unavailable')
        self.read_until(lambda value:value=='READY',35)

    def line(self,seconds):
        until=time.monotonic()+seconds
        while b'\n' not in self.buffer:
            remaining=until-time.monotonic()
            if remaining<=0:raise RuntimeError('probe output deadline exceeded')
            ready,_,_=select.select([self.probe.stdout],[],[],remaining)
            if not ready:continue
            block=os.read(self.probe.stdout.fileno(),65536)
            if not block:raise RuntimeError('probe exited before required actual result')
            self.buffer+=block
        line,self.buffer=self.buffer.split(b'\n',1);line=line.decode()
        self.probe_output.write(line+'\n');self.probe_output.flush()
        if line.startswith('RESULT '):
            value=json.loads(line[7:]);self.results.append(value);return value
        return line

    def read_until(self,predicate,seconds=35):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            value=self.line(until-time.monotonic())
            if predicate(value):return value
        raise RuntimeError('required probe result missing')

    def command(self,text,predicate=lambda value:isinstance(value,dict),seconds=35):
        self.probe.stdin.write((text+'\n').encode());self.probe.stdin.flush()
        return self.read_until(predicate,seconds)

    def reap(self,child,seconds):
        def exited():
            found,raw=os.waitpid(child.pid,os.WNOHANG)
            if found:
                child.returncode=os.waitstatus_to_exitcode(raw);self.raw[str(child.pid)]=raw
                (self.logs/'raw-wait-statuses.json').write_text(json.dumps(self.raw,indent=2));return raw+1
        wait(exited,seconds,'actual child wait status not observed')
        return self.raw[str(child.pid)]

    def finish_probe(self,expect_clean):
        if expect_clean:
            value=self.command('CLEAN',lambda value:isinstance(value,dict) and 'clean_status' in value)
            if value['clean_status']!=0:raise RuntimeError('original native probe did not clean after matching late ACK')
            snap=self.read_until(lambda value:isinstance(value,dict) and 'initialized' in value)
            if not(snap['initialized']==1 and snap['finalized']==1 and snap['complete'] is True
                   and snap['abt_uninitialized'] is True and snap['primary_releases']==1):
                raise RuntimeError('original native probe physical release incomplete')
        self.probe.stdin.write(b'QUIT\n');self.probe.stdin.flush()
        if self.reap(self.probe,5)!=0:raise RuntimeError('probe did not exit normally')

    def run(self):
        visor,_=self.service('visor')
        if self.args.case in ('startup-main','startup-dead'):
            self.peer_signal(signal.SIGSTOP)
            keeper,entered=self.service('keeper')
            status=Path(str(self.children['keeper'][1])+'.lifecycle.json')
            if self.args.case=='startup-dead':
                wait(lambda:self.trace('register_keeper',keeper.pid),10,'original registration did not dispatch')
                self.peer_signal(signal.SIGKILL);self.reap(visor,5)
            state=wait(lambda:status.exists() and self.state('keeper').get('state') in ('Quiescing','Draining')
                and self.state('keeper'),33,'actual Keeper startup did not leave retries by original30s')
            observed_ms=int((time.monotonic()-entered)*1000)
            (self.logs/'startup-incomplete-native.json').write_text(json.dumps(state,indent=2))
            if not 28500<=observed_ms<=33000:raise RuntimeError('actual startup total missed default30s bound')
            proof=state['native_proof']
            if proof['initialized_mids']!=3 or proof['finalized_mids']!=0 or identity(keeper.pid)!=self.owned[keeper.pid]:
                raise RuntimeError('startup expiry lost/replaced original native3MID owner')
            original=self.trace('register_keeper',keeper.pid)
            if len(original)!=1:raise RuntimeError('startup replaced or never dispatched original registration')
            if self.args.case=='startup-dead':
                # Death is not settlement. The same unknown registration/owner stays live.
                if state['status']!=-27 or proof['complete'] or len(self.trace('register_keeper',keeper.pid))!=1:
                    raise RuntimeError('dead registration falsely retired/replaced original native generation')
                return {'startup_observed_ms':observed_ms,'same_register_forward':original,
                    'retained_native':proof,'peer_raw_wait':self.raw[str(visor.pid)],
                    'scope':'actual main default30 startup with dead peer; retained unknown, fixture disposal only'}
            self.peer_signal(signal.SIGCONT)
            wait(lambda:self.trace('register_keeper',keeper.pid,'forward_return'),10,'original registration did not return after real Visor resume')
            raw=self.reap(keeper,32)
            final=self.state('keeper');(self.logs/'startup-final-native.json').write_text(json.dumps(final,indent=2))
            if raw!=256 or not final['native_proof']['complete'] or len(self.trace('register_keeper',keeper.pid))!=1:
                raise RuntimeError('startup retry/cleanup did not preserve original call/failed-start result')
            return {'startup_observed_ms':observed_ms,'same_register_forward':original,'keeper_raw_wait':raw,
                    'keeper_native':final['native_proof'],'scope':'actual main startup total; later known cleanup'}
        if self.args.case=='recoverable':
            self.peer_signal(signal.SIGSTOP);self.launch_probe('startup')
            total=next(row for row in self.results if 'startup_elapsed_ms' in row)
            calls=[row for row in self.results if row.get('action')==0]
            if not 28500<=total['startup_elapsed_ms']<=33000 or not calls:
                raise RuntimeError('default startup shared budget not measured')
            first=calls[0]['completion'];mid=calls[0]['mid']
            if not first or any(row['completion']!=first or row['mid']!=mid for row in calls):
                raise RuntimeError('registration replaced original completion/MID')
            if not 3500<=calls[0]['elapsed_ms']<=6500:raise RuntimeError('first registration caller did not use default5s')
            expired=self.command('EXPIRED')
            if expired['status']!=-22 or expired['elapsed_ms']>250:raise RuntimeError('expired original entry dispatched/restarted budget')
            snap=self.command('SNAP')
            if snap['available_C2']!=1 or snap['finalized']!=0 or not snap['owner_retained']:
                raise RuntimeError('expired registration lost its original C2 charge/native owner')
            self.peer_signal(signal.SIGCONT)
            wait(lambda:self.trace('register_keeper',self.probe.pid,'forward_return'),10,'original registration did not settle after resume')
            known=self.command('REGISTER')
            if known['status']!=0 or known['completion']!=first or len(self.trace('register_keeper',self.probe.pid))!=1:
                raise RuntimeError('late register ACK caused duplicate dispatch/replacement')
            self.peer_signal(signal.SIGSTOP)
            self.probe.stdin.write(b'UNREGISTER_STOP\n');self.probe.stdin.flush()
            total_stop=self.read_until(lambda value:isinstance(value,dict) and 'stop_elapsed_ms' in value,33)
            unregs=[row for row in self.results if row.get('action')==2]
            if not 28500<=total_stop['stop_elapsed_ms']<=33000 or not unregs:
                raise RuntimeError('unregister did not share actual original30s stop entry')
            original=unregs[0]['completion']
            if not 3500<=unregs[0]['elapsed_ms']<=6500 or any(row['completion']!=original or row['mid']!=mid for row in unregs):
                raise RuntimeError('unregister changed default5s/original completion/MID')
            snap=self.command('DROP')
            if not snap['owner_retained'] or snap['available_C2']!=1 or snap['finalized']!=0:
                raise RuntimeError('dropping facade reclaimed unresolved original call/native owner')
            incomplete=self.command('DRAIN',lambda value:isinstance(value,dict) and 'drain_status' in value)
            retained=self.read_until(lambda value:isinstance(value,dict) and 'initialized' in value)
            if incomplete['drain_status']!=-27 or retained['complete'] or retained['finalized']!=0:
                raise RuntimeError('caller expiry falsely settled original unregister/native state')
            self.peer_signal(signal.SIGCONT)
            wait(lambda:self.trace('unregister_keeper',self.probe.pid,'forward_return'),10,'original unregister did not finish')
            if len(self.trace('unregister_keeper',self.probe.pid))!=1:raise RuntimeError('duplicate unregister dispatch')
            self.finish_probe(True)
            return {'startup':total,'stop':total_stop,'registration_completion':first,'unregister_completion':original,
                    'mid':mid,'scope':'same original5s/30s caller records; matching late ACK and physical cleanup'}
        self.launch_probe('session')
        if self.args.case=='lock':
            self.peer_signal(signal.SIGSTOP)
            self.command('LOCK',lambda value:value=='LOCKED')
            time.sleep(4.2)
            result=self.command('UNLOCK')
            if result['locked_status']!=-22 or not 3500<=result['locked_elapsed_ms']<=6500:
                raise RuntimeError('lock preparation reset original default5s budget')
            self.peer_signal(signal.SIGCONT)
            wait(lambda:self.trace('unregister_keeper',self.probe.pid,'forward_return'),10,'held original call did not return')
            self.finish_probe(True)
            return {'lock_result':result,'scope':'4.2s actual mutex wait consumes original5s; no budget reset'}
        if self.args.case!='dead':raise RuntimeError('unsupported case')
        self.peer_signal(signal.SIGSTOP)
        first=self.command('UNREGISTER')
        if first['status']!=-22 or not 3500<=first['elapsed_ms']<=6500:raise RuntimeError('unregister did not time out at actual default5s')
        self.peer_signal(signal.SIGKILL);self.reap(visor,5) # Only this new fixture peer; never retirement proof.
        before=self.command('SNAP');again=self.command('UNREGISTER')
        after=self.command('DROP')
        if (again['completion']!=first['completion'] or before['mid']!=after['mid']
            or after['available_C2']!=1 or after['finalized']!=0 or not after['owner_retained']
            or after['unregister'].get('known') is True or len(self.trace('unregister_keeper',self.probe.pid))!=1):
            raise RuntimeError('dead peer reclaimed/replaced original unknown unregister')
        drained=self.command('DRAIN',lambda value:isinstance(value,dict) and 'drain_status' in value,33)
        retained=self.read_until(lambda value:isinstance(value,dict) and 'initialized' in value)
        if drained['drain_status']!=-27 or retained['complete'] or retained['finalized']!=0:
            raise RuntimeError('peer death falsely proved native retirement')
        self.finish_probe(False)
        return {'first':first,'again':again,'retained':retained,'drain':drained,
                'scope':'genuine unknown retained despite peer exit; probe process exit is fixture disposal, not settlement'}

    def cleanup(self):
        if self.probe and self.probe.returncode is None:
            if identity(self.probe.pid)!=self.probe_identity:raise RuntimeError('probe identity changed before fixture disposal')
            signal_owned(self.probe.pid,self.probe_identity,signal.SIGTERM)
            try:self.probe.wait(timeout=3)
            except subprocess.TimeoutExpired:
                signal_owned(self.probe.pid,self.probe_identity,signal.SIGKILL);self.probe.wait(timeout=3)
        self.probe_output.close();self.probe_error.close()
        # Already reaped normal or deliberately dead children are not signalled.
        live={pid:value for pid,value in self.owned.items() if identity(pid)==value}
        dispose_fixture(live,self.work,self.logs)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--protocol',choices=('ofi+sockets','ofi+tcp'),required=True)
    parser.add_argument('--case',choices=('startup-main','startup-dead','recoverable','lock','dead'),required=True)
    parser.add_argument('--logs',required=True);args=parser.parse_args();fixture=Fixture(args)
    try:
        result=fixture.run()
        print(json.dumps({'case':args.case,'protocol':args.protocol,'status':'PASS','trace':False,'passive_native_spy':True,
            'result':result,'raw_wait_statuses':fixture.raw}),flush=True)
    finally:fixture.cleanup()
