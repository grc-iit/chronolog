# No-Sudo Install Strategy

Status: complete.

Evidence directory: `.agent/results/20260511-220041/`

Phase 0 assumes no sudo access. Dependencies and profiling tools must be detected or installed in this order:

1. Existing command on `PATH`.
2. Existing cluster module.
3. User-local install under `$HOME/.local`.
4. Project-local install under repository `opt/`.
5. Conda/mamba/spack when available without sudo.
6. Source build into a user-local or project-local prefix.

## Detected Installation Channels

| Channel | Status | Evidence |
|---|---|---|
| Existing commands | available | `git`, `curl`, `wget`, `cmake`, `make`, `ninja`, `pip3` found on `PATH` |
| Cluster modules | available | `module`/`ml` available; 1203 module entries observed |
| `$HOME/.local` | available | `/home/jcernudagarcia/.local` exists |
| Project `opt/` | available on demand | `/home/jcernudagarcia/chronolog-opt/chronolog/opt` missing; create only when needed |
| Spack | available | `spack` 0.23.1 found; environment `mchips` listed |
| Conda/mamba | unavailable | `conda`, `mamba`, and `micromamba` missing from `PATH` |
| Source builds | available | `git`, `curl`, `wget`, `cmake`, `make`, `ninja`, `tar`, and `unzip` found |

## Tool-Specific Preference

| Tool area | First choice | Fallback |
|---|---|---|
| ChronoLog build dependencies | Cluster modules or existing Spack environment | Project-local Spack/source build under `opt/` |
| MPI wrappers | Load `openmpi` or `mpich` module | Spack install into `$HOME/.local` or `opt/` |
| TAU | Existing command/module if present during tool detection | Source build into `$HOME/.local` or `opt/tau` |
| `perf` | Existing OS command | Blocked if unavailable and kernel package requires sudo |
| gperftools | Existing command/module if present | Source build into `$HOME/.local` or `opt/gperftools` |
| Darshan | `darshan-runtime`/`darshan-util` modules | Source build into `$HOME/.local` or `opt/darshan` |
| eBPF-based tools | Existing `bpftrace`/BCC tools if permitted | Blocked if kernel permissions or package install requires sudo |
| Linux network measurement commands | Existing commands/modules | User-local/source install only where practical; kernel/NIC privileged operations may be read-only |

## Blocker Rule

A dependency is blocked only after documenting attempts through available command detection, module detection, user-local/project-local installation, Spack or source-build paths, and any permission limitation. Lack of sudo alone is not a blocker.
