# ChronoLog Distributed Profiling Gap Wrap-up

Status: fixed for all non-blocked collectors.

Validated distributed runs:

| Collector | Result directory | Validated output |
|---|---|---|
| TAU | `.agent/results/20260512-094406` | client and service `profile.*` files under `chronolog/profiles/tau` |
| gperftools | `.agent/results/20260512-094726` | service CPU profiles and heap profiles under `chronolog/profiles/gperftools` |
| Darshan | `.agent/results/20260512-095335` | client and service `.darshan` logs under `chronolog/profiles/darshan` |

Script fixes made during validation:

- Canonicalized `--install-dir` so remote service wrappers do not receive relative paths.
- Passed gperftools and Darshan client instrumentation through MPI environment forwarding so `mpirun` itself is not preloaded.
- Removed unavailable Darshan module load from remote wrappers and used the known runtime library path directly.
- Enabled `DARSHAN_ENABLE_NONMPI=1` for service-role Darshan capture.
- Increased profiled startup wait time so services can register before the client starts.

The generated static package is under `profiling/0`.
