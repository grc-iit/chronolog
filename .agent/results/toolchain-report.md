# Phase 0 Toolchain Report

Status: complete.

## Branch and Scheduler

- Branch: `opt/phase0-bootstrap`
- Branch base: `origin/develop` is an ancestor of `HEAD`
- SLURM commands: `sinfo`, `squeue`, `scontrol`, `sbatch`, `srun`, `sacct`
- Default partition: `compute`
- Additional partitions: `datacrumbs`, `debug`
- Useful validation partition: `debug` when idle
- Evidence: `.agent/results/slurm-environment.md`

## No-Sudo Strategy

Dependency handling order:

1. Existing command on `PATH`
2. Cluster module
3. `$HOME/.local`
4. Repository `opt/`
5. Conda/mamba/spack where available
6. Source build into a user-local or project-local prefix

Available channels:

- Modules: available
- Spack: available, version 0.23.1
- `$HOME/.local`: available
- Repository `opt/`: used for local TAU and perf installs
- Conda/mamba/micromamba: unavailable

Evidence: `.agent/results/no-sudo-install-strategy.md`

## Compiler and Build Tools

| Tool | Status |
|---|---|
| `gcc` | `/usr/bin/gcc`, Ubuntu 11.4.0 |
| `g++` | `/usr/bin/g++`, Ubuntu 11.4.0 |
| `cc` | `/usr/bin/cc`, Ubuntu 11.4.0 |
| `c++` | `/usr/bin/c++`, Ubuntu 11.4.0 |
| `cmake` | `/usr/bin/cmake`, 3.22.1; module `cmake/3.30.5-gcc-11.4.0-7bxr5u6` available |
| `make` | `/usr/bin/make`, 4.3 |
| `ninja` | `/usr/bin/ninja`, 1.10.1 |
| MPI wrappers | missing on initial PATH; `openmpi/5.0.5-gcc-11.4.0-og56sxz` and `mpich/4.1.1` modules available |

## ChronoLog Dependency Modules

Observed relevant module candidates:

- `argobots/1.1-gcc-11.4.0-indjy5q`
- `mercury/2.3.1-gcc-11.4.0-v35oegb`
- `mochi-margo/0.17.0-gcc-11.4.0-l6v4nr7`
- `boost/1.86.0-gcc-11.4.0-cs4onpk`
- `hdf5/1.14.5-gcc-11.4.0-flto63r`
- `openmpi/5.0.5-gcc-11.4.0-og56sxz`

## Profiling and Observability Stack

### TAU

Status: installed locally.

- Prefix: `opt/tau-2.34`
- Environment: `.agent/config/tau-env.sh`
- C wrapper: `opt/tau-2.34/x86_64/bin/tau_cc.sh`
- C++ wrapper: `opt/tau-2.34/x86_64/bin/tau_cxx.sh`
- Makefile: `opt/tau-2.34/x86_64/lib/Makefile.tau-pthread`
- Validation: wrappers resolve compile/link commands with `TAU_MAKEFILE`
- Evidence: `.agent/results/tau-install.md`

### perf

Status: installed locally with runtime permission limitation.

- Binary: `opt/perf/extract/usr/lib/linux-tools-5.15.0-176/perf`
- Environment: `.agent/config/perf-env.sh`
- Version: 5.15.199
- Limitation: `perf stat` is blocked by `perf_event_paranoid=4`
- Evidence: `.agent/results/perf-install.md`

### gperftools

Status: installed system-wide and validated.

- `google-pprof`: `/usr/bin/google-pprof`
- Headers: `/usr/include/gperftools`
- Libraries: `/usr/lib/x86_64-linux-gnu/libprofiler.so`, `/usr/lib/x86_64-linux-gnu/libtcmalloc.so`
- Validation: C++ probe produced CPU and heap profiles
- Evidence: `.agent/results/gperftools-detection.md`

### Darshan

Status: available via modules.

- Runtime module: `darshan-runtime/3.4.6-gcc-11.4.0-u7vfz6e`
- Utility module: `darshan-util/3.4.6-gcc-11.4.0-75rttfw`
- Environment: `.agent/config/darshan-env.sh`
- Validation: `darshan-config` reports log path and link flags
- Evidence: `.agent/results/darshan-detection.md`

### eBPF-based tools

Status: detected with runtime permission limitations.

- Module: `bpftool/7.5.0`
- Local tool: `opt/perf/extract/usr/lib/linux-tools-5.15.0-176/bpftool`
- Missing: bpftrace and BCC tools
- Limitation: `unprivileged_bpf_disabled=2`, `perf_event_paranoid=4`, root-only BPF/debug/tracing filesystems
- Evidence: `.agent/results/ebpf-based-tools-detection.md`

### Linux network measurement commands

Status: installed and smoke-tested.

| Command | Path |
|---|---|
| `iperf3` | `/usr/bin/iperf3` |
| `ss` | `/usr/bin/ss` |
| `nstat` | `/usr/bin/nstat` |
| `sar -n` | `/usr/bin/sar` |
| `ethtool` | `/usr/sbin/ethtool` |
| `tcpdump` | `/usr/bin/tcpdump` |

Evidence: `.agent/results/linux-network-measurement-commands.md`

## Known Limitations

- `perf` runtime profiling is blocked in the current environment by `perf_event_paranoid=4`.
- eBPF-based observability is blocked for normal user execution by kernel settings and root-only trace/debug/BPF filesystems.
- `ethtool` read-only link checks work, but some netlink operations report `Operation not permitted`.
- TAU and perf local installs are under ignored `opt/` and must remain present on this workspace or be recreated from the recorded commands.
