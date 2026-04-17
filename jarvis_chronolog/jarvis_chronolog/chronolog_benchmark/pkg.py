"""
ChronoLog Benchmark / smoke-test

Runs `chimaera monitor --once --json` on every node in the hostfile to
verify that the IOWarp runtime is alive and that the ChronoLog chimod
pools (keeper, grapher, player, store) are registered. The JSON snapshot
doubles as a lightweight before/after benchmark record.

`custom_cmd` is the extension point for a real MPI-launched workload
driver — it is run through `MpiExecInfo`, so jarvis builds the proper
`mpirun -np ... -ppn ...` line and wraps the whole invocation in
`docker exec` when the pipeline is containerised.
"""
import os

from jarvis_cd.core.pkg import Application
from jarvis_cd.shell import Exec, MpiExecInfo, PsshExecInfo


class ChronologBenchmark(Application):
    """Verify a deployed ChronoLog stack and optionally drive a custom binary."""

    def _configure_menu(self):
        return [
            {
                'name': 'output_dir',
                'msg': (
                    'Directory where monitor snapshots and logs are written '
                    '(must live under a mounted path when running in container '
                    'mode; empty string = <shared_dir>/benchmark_out)'
                ),
                'type': str,
                'default': '',
            },
            {
                'name': 'custom_cmd',
                'msg': (
                    'Optional extra command to run after the smoke-test '
                    '(e.g. an MPI-launched ChronoLog client binary). '
                    'Empty string disables.'
                ),
                'type': str,
                'default': '',
            },
            {
                'name': 'custom_nprocs',
                'msg': 'Total MPI processes for custom_cmd',
                'type': int,
                'default': 1,
            },
            {
                'name': 'custom_ppn',
                'msg': 'Processes per node for custom_cmd',
                'type': int,
                'default': 1,
            },
        ]

    def _output_dir(self) -> str:
        cfg = self.config.get('output_dir', '') or ''
        return cfg or os.path.join(self.shared_dir, 'benchmark_out')

    def _container_kwargs(self):
        return {
            'container': self._container_engine,
            'container_image': self.deploy_image_name(),
            'private_dir': self.private_dir,
            'bind_mounts': self.container_mounts,
        }

    def _snapshot(self, tag: str):
        out_dir = self._output_dir()
        out = os.path.join(out_dir, f'chimaera_monitor.{tag}.json')
        cmd = f'mkdir -p {out_dir} && chimaera monitor --once --json > {out} 2>&1'
        self.log(f'Capturing chimaera monitor snapshot -> {out}')
        Exec(cmd, PsshExecInfo(
            env=self.mod_env,
            hostfile=self.jarvis.hostfile,
            **self._container_kwargs(),
        )).run()

    def _run_custom(self):
        raw = self.config.get('custom_cmd', '').strip()
        if not raw:
            return
        out_dir = self._output_dir()
        log = os.path.join(out_dir, 'custom_cmd.log')
        Exec(f'mkdir -p {out_dir}', PsshExecInfo(
            env=self.mod_env,
            hostfile=self.jarvis.hostfile,
            **self._container_kwargs(),
        )).run()
        cmd = f'{raw} 2>&1 | tee {log}'
        self.log(f'Running custom benchmark command under mpirun: {raw}')
        Exec(cmd, MpiExecInfo(
            env=self.mod_env,
            hostfile=self.jarvis.hostfile,
            nprocs=self.config['custom_nprocs'],
            ppn=self.config['custom_ppn'],
            **self._container_kwargs(),
        )).run()

    def start(self):
        self._snapshot('pre')
        self._run_custom()
        self._snapshot('post')
        self.log(f'Benchmark outputs in {self._output_dir()}')

    def stop(self):
        pass

    def clean(self):
        out_dir = self._output_dir()
        self.log(f'Removing benchmark output at {out_dir}')
        Exec(f'rm -rf {out_dir}', PsshExecInfo(
            env=self.mod_env,
            hostfile=self.jarvis.hostfile,
            **self._container_kwargs(),
        )).run()

    def status(self):
        return 'unknown'
