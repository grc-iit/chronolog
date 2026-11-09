#!/usr/bin/env python3
"""Hold or fail a daemon's own lifecycle status write in its rename.

One unmanaged chrono-visor runs from the install tree with the shipped
configuration (optionally a shorter shutdown budget) and a preloaded rename
gate (librpc_lifecycle_status_hold.so). Cases:

  quiescing-hold  The Quiescing write of TERM 1 is held. TERM 1 and TERM 2
                  each return at their own deadline with the daemon alive and
                  no new threads. After release the held write lands as
                  attempt 1, and TERM 3 stops the daemon as attempt 3.
  stopped-hold    The Stopped write from the C32 cleanup callback is held.
                  TERM 1 returns at its deadline; TERM 2 does not repeat the
                  write. The daemon exits within TERM 2 once it is released.
  stopped-fail    The first Stopped rename fails with EIO. TERM 1 returns at
                  its deadline; TERM 2 writes the same record once more and
                  the daemon exits as attempt 2.

Needs WORK_DIR (install tree) and BUILD_DIR (build tree with the shim).
Only processes this fixture launched are signalled.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time
from rpc_lifecycle_deadline_test import identity, signal_owned, patch, fixture_processes, dispose_fixture


def wait(predicate, seconds, reason):
    until = time.monotonic()+seconds
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(.01)
    raise RuntimeError(reason)


def sleep_until(moment):
    remaining = moment-time.monotonic()
    if remaining > 0:
        time.sleep(remaining)


class Fixture:
    def __init__(self, args):
        self.args = args
        self.logs = Path(args.logs).resolve()
        self.logs.mkdir(parents=True, exist_ok=False)
        shutil.copy2(__file__, self.logs/'executed-fixture.py')
        installed = Path(os.environ['WORK_DIR']).resolve()
        build = Path(os.environ['BUILD_DIR']).resolve()
        self.work = self.logs/'runtime-work'
        (self.work/'conf').mkdir(parents=True)
        (self.work/'monitor').mkdir()
        for name in ('bin', 'lib', 'tools'):
            (self.work/name).symlink_to(installed/name, target_is_directory=True)
        if fixture_processes(self.work):
            raise RuntimeError('native fixture executables already occupied')
        configuration = json.loads((installed/'conf/default-chrono-conf.json').read_text())
        patch(configuration, args.protocol, False, False)
        visor = configuration['chrono_visor']
        policy = visor['RpcPolicy']
        if (policy['attempt_ms'], policy['startup_ms'], policy['shutdown_ms']) != (5000, 30000, 30000):
            raise RuntimeError('requires the shipped lifecycle budgets')
        if args.shutdown_ms:
            policy['shutdown_ms'] = args.shutdown_ms
        self.budget = policy['shutdown_ms']/1000
        visor['Monitoring']['monitor']['file'] = str(self.work/'monitor/chrono-visor.log')
        self.config = self.work/'conf/chrono-visor-conf.json'
        self.config.write_text(json.dumps(configuration, indent=2))
        self.status = Path(str(self.config)+'.lifecycle.json')
        self.gate = self.logs/'gate'
        self.env = dict(os.environ)
        for key in ('CHRONOLOG_DEPLOYMENT_ID', 'CHRONOLOG_DEPLOYMENT_ROOT', 'CHRONOLOG_DEPLOYMENT_GUARD'):
            self.env.pop(key, None)
        # Native loopback fixture: never used for distributed runs.
        self.env['FI_SOCKETS_IFACE'] = 'lo'
        self.env['FI_TCP_IFACE'] = 'lo'
        self.env['LD_PRELOAD'] = str(build/'tests/integration/watermark-replay/librpc_lifecycle_status_hold.so')
        self.env['CHRONOLOG_TEST_STATUS_GATE'] = str(self.gate)
        if args.case == 'quiescing-hold':
            self.env['CHRONOLOG_TEST_STATUS_HOLD_STATE'] = 'Quiescing'
        elif args.case == 'stopped-hold':
            self.env['CHRONOLOG_TEST_STATUS_HOLD_STATE'] = 'Stopped'
        else:
            self.env['CHRONOLOG_TEST_STATUS_FAIL_STATE'] = 'Stopped'
        self.child = None
        self.owned = {}
        self.events = []

    def note(self, name, **fields):
        self.events.append(dict(event=name, t=round(time.monotonic()-self.started, 3), **fields))

    def state(self):
        return json.loads(self.status.read_text())

    def threads(self):
        return len(os.listdir('/proc/%d/task' % self.child.pid))

    def alive(self):
        return self.child.poll() is None and identity(self.child.pid) == self.owned[self.child.pid]

    def renames(self, state):
        path = Path(str(self.gate)+'.renames')
        rows = path.read_text().splitlines() if path.exists() else []
        return sum(1 for row in rows if row.split()[-1:] == [state])

    def term(self):
        signal_owned(self.child.pid, self.owned[self.child.pid], signal.SIGTERM)
        return time.monotonic()

    def reap(self, seconds):
        def exited():
            found, raw = os.waitpid(self.child.pid, os.WNOHANG)
            if found:
                self.child.returncode = os.waitstatus_to_exitcode(raw)
                return (raw, time.monotonic())
        return wait(exited, seconds, 'daemon did not exit')

    def launch(self):
        with (self.work/'monitor/chrono-visor.launch.log').open('w') as output:
            self.child = subprocess.Popen([str(self.work/'bin/chrono-visor'), '--config', str(self.config)],
                                          env=self.env, cwd=self.work, stdout=output, stderr=subprocess.STDOUT)
        self.started = time.monotonic()
        binary = str((self.work/'bin/chrono-visor').resolve())
        # Capture the identity only after exec has finished: before it the child
        # is a fork of this script, and during it cmdline can still be empty.
        def executed():
            found = identity(self.child.pid)
            return found if found and found[0] == binary and b'--config' in found[1] else None
        self.owned[self.child.pid] = wait(executed, 5, 'daemon identity unavailable')
        wait(lambda: self.status.exists() and self.state().get('state') == 'Running', 32, 'Visor did not reach Running')
        self.note('running', threads=self.threads())

    def require(self, condition, reason):
        if not condition:
            raise RuntimeError(reason+': '+json.dumps(self.events))

    def stopped(self, raw, attempt):
        final = self.state()
        proof = final.get('native_proof', {})
        self.note('exit', raw=raw, record=final)
        self.require(raw == 0, 'daemon exit status is not 0')
        self.require(final.get('state') == 'Stopped' and final.get('status') == 0
                     and final.get('attempt') == attempt and final.get('pid') == self.child.pid,
                     'Stopped record does not name attempt %d' % attempt)
        self.require(proof.get('initialized_mids') == 2 and proof.get('finalized_mids') == 2
                     and proof.get('primary_releases') == 1 and proof.get('complete') is True
                     and proof.get('abt_uninitialized') is True, 'Stopped record lacks physical native retirement')

    def quiescing_hold(self):
        threads = self.threads()
        first = self.term()
        wait(lambda: Path(str(self.gate)+'.entered').exists(), 5, 'Quiescing rename was not entered')
        self.require(self.state().get('state') == 'Running', 'held Quiescing write replaced the record')
        sleep_until(first+self.budget+.5)
        self.note('after-first-deadline', alive=self.alive(), threads=self.threads(), record=self.state())
        self.require(self.alive() and self.state().get('state') == 'Running', 'attempt 1 changed state while held')
        self.require(self.threads() == threads, 'held write grew the thread count')
        # Only a coordinator that returned from attempt 1 can start attempt 2.
        second = self.term()
        sleep_until(second+self.budget+.5)
        self.note('after-second-deadline', alive=self.alive(), threads=self.threads(), record=self.state())
        self.require(self.alive() and self.state().get('state') == 'Running', 'attempt 2 changed state while held')
        self.require(self.threads() == threads, 'retry grew the thread count')
        self.require(self.renames('Quiescing') == 1, 'a second Quiescing write was admitted while one was held')
        (Path(str(self.gate)+'.release')).touch()
        wait(lambda: Path(str(self.gate)+'.completed').exists(), 5, 'held rename did not finish')
        self.require(Path(str(self.gate)+'.completed').read_text().strip() == '0', 'held rename failed')
        held = wait(lambda: self.state().get('state') == 'Quiescing' and self.state(), 5, 'held record not in place')
        self.note('released', record=held)
        self.require(held.get('attempt') == 1, 'the held write is not the attempt 1 record')
        third = self.term()
        raw, ended = self.reap(self.budget+5)
        self.stopped(raw, 3)
        self.require(self.renames('Quiescing') == 2 and self.renames('Stopped') == 1, 'unexpected write count')
        return {'third_term_to_exit_ms': int((ended-third)*1000), 'threads': threads}

    def stopped_hold(self):
        threads = self.threads()
        first = self.term()
        wait(lambda: Path(str(self.gate)+'.entered').exists(), 10, 'Stopped rename was not entered')
        record = self.state()
        self.require(record.get('state') == 'Quiescing' and record.get('attempt') == 1, 'Quiescing record missing')
        sleep_until(first+self.budget+.5)
        self.note('after-first-deadline', alive=self.alive(), threads=self.threads(), record=self.state())
        self.require(self.alive() and self.state().get('state') == 'Quiescing', 'attempt 1 ended while Stopped held')
        second = self.term()
        time.sleep(1)
        self.require(self.alive() and self.renames('Stopped') == 1, 'held Stopped write was repeated')
        self.require(self.threads() <= threads, 'held Stopped write grew the thread count')
        released = time.monotonic()
        (Path(str(self.gate)+'.release')).touch()
        raw, ended = self.reap(self.budget+5)
        # The write began in attempt 1; attempt 2 observes its completion.
        self.stopped(raw, 1)
        self.require(ended-second < self.budget, 'attempt 2 did not observe the released write')
        self.require(self.renames('Stopped') == 1, 'Stopped write was repeated')
        return {'release_to_exit_ms': int((ended-released)*1000)}

    def stopped_fail(self):
        first = self.term()
        wait(lambda: Path(str(self.gate)+'.failed').exists(), 10, 'Stopped rename did not fail')
        sleep_until(first+self.budget+.5)
        self.note('after-first-deadline', alive=self.alive(), record=self.state())
        self.require(self.alive() and self.state().get('state') == 'Quiescing', 'failed Stopped write ended attempt 1')
        self.require(self.renames('Stopped') == 1, 'failed write retried without a new stop request')
        second = self.term()
        raw, ended = self.reap(self.budget+5)
        self.stopped(raw, 2)
        self.require(self.renames('Stopped') == 2, 'failed write not retried exactly once')
        return {'second_term_to_exit_ms': int((ended-second)*1000)}

    def run(self):
        self.launch()
        result = getattr(self, self.args.case.replace('-', '_'))()
        (self.logs/'events.json').write_text(json.dumps(self.events, indent=2))
        return result

    def cleanup(self):
        (self.logs/'events.json').write_text(json.dumps(self.events, indent=2))
        if self.child is not None:
            (Path(str(self.gate)+'.release')).touch()
            live = {pid: value for pid, value in self.owned.items() if identity(pid) == value}
            dispose_fixture(live, self.work, self.logs)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--protocol', choices=('ofi+sockets', 'ofi+tcp'), required=True)
    parser.add_argument('--case', choices=('quiescing-hold', 'stopped-hold', 'stopped-fail'), required=True)
    parser.add_argument('--shutdown-ms', type=int, default=0, help='replace the shipped 30000 ms Visor budget')
    parser.add_argument('--logs', required=True)
    args = parser.parse_args()
    fixture = Fixture(args)
    try:
        result = fixture.run()
        print(json.dumps({'case': args.case, 'protocol': args.protocol, 'status': 'PASS',
                          'shutdown_ms': args.shutdown_ms or 30000, 'result': result}), flush=True)
    finally:
        fixture.cleanup()
