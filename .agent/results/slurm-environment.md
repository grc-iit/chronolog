# SLURM Environment Discovery

Status: complete.

Evidence directory: `.agent/results/20260511-215928/`

## Scheduler Commands

| Command | Status |
|---|---|
| `sinfo` | `/usr/bin/sinfo` |
| `squeue` | `/usr/bin/squeue` |
| `scontrol` | `/usr/bin/scontrol` |
| `sbatch` | `/usr/bin/sbatch` |
| `srun` | `/usr/bin/srun` |
| `sacct` | `/usr/bin/sacct` |

## Partitions

| Partition | Default | Total nodes | CPUs | Max time | Observed state summary |
|---|---:|---:|---:|---|---|
| `compute` | yes | 22 | 880 | `2-00:00:00` | 2 mixed, 20 allocated |
| `datacrumbs` | no | 29 | 1160 | `2-00:00:00` | 5 idle, 2 mixed, 20 allocated, 1 invalid, 1 down |
| `debug` | no | 6 | 240 | `4-00:00:00` | 5 idle, 1 invalid |

Per-node shape from `sinfo`/`lscpu`: 40 CPUs, 47759 MB memory, Intel Xeon Silver 4114, 2 sockets, 10 cores per socket, 2 threads per core.

## Job Limits

`scontrol show partition` reports `MaxNodes=UNLIMITED`, `MaxCPUsPerNode=UNLIMITED`, `DefMemPerNode=UNLIMITED`, and `MaxMemPerNode=UNLIMITED` for `compute`, `datacrumbs`, and `debug`. Practical Phase 0 scaling remains constrained by current node availability and scheduler policy.

No active jobs were listed for the current user during discovery.

## Modules

Environment modules are available. No modules were loaded during discovery.

Relevant module candidates observed:

| Area | Module candidates |
|---|---|
| Compiler/build | `cmake/3.30.5-gcc-11.4.0-7bxr5u6`, `gmake/4.4.1-gcc-11.4.0-fxk44dg` |
| MPI | `openmpi/5.0.5-gcc-11.4.0-og56sxz`, `mpich/4.1.1` |
| ChronoLog dependencies | `argobots/1.1-gcc-11.4.0-indjy5q`, `mercury/2.3.1-gcc-11.4.0-v35oegb`, `mochi-margo/0.17.0-gcc-11.4.0-l6v4nr7`, `boost/1.86.0-gcc-11.4.0-cs4onpk`, `hdf5/1.14.5-gcc-11.4.0-flto63r` |
| Profiling/I/O | `darshan-runtime/3.4.6-gcc-11.4.0-u7vfz6e`, `darshan-util/3.4.6-gcc-11.4.0-75rttfw` |

## Compiler Stack

| Tool | Status |
|---|---|
| `gcc` | `/usr/bin/gcc`, Ubuntu 11.4.0 |
| `g++` | `/usr/bin/g++`, Ubuntu 11.4.0 |
| `cc` | `/usr/bin/cc`, Ubuntu 11.4.0 |
| `c++` | `/usr/bin/c++`, Ubuntu 11.4.0 |
| `mpicc` | missing from current PATH; MPI module required |
| `mpicxx` | missing from current PATH; MPI module required |
| `cmake` | `/usr/bin/cmake`, 3.22.1 |
| `make` | `/usr/bin/make`, 4.3 |
| `ninja` | `/usr/bin/ninja`, 1.10.1 |

## Notes For Phase 0

- Prefer scheduler partition `debug` for short validation jobs when available.
- Use the `compute` partition for normal runs once scripts exist.
- Load an MPI module before ChronoLog builds that require MPI compiler wrappers.
- Use module-provided Darshan before considering user-local/source installation.
