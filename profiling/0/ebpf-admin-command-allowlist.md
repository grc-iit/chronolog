# eBPF-Based Tool Allowlist Request

Target nodes:

- `ares-comp-03`
- `ares-comp-04`
- `ares-comp-05`
- `ares-comp-06`

Current status:

- `kernel.perf_event_paranoid=1` on `ares-comp-03` and `ares-comp-04`.
- `kernel.yama.ptrace_scope=0` on `ares-comp-03` and `ares-comp-04`.
- `kernel.unprivileged_bpf_disabled=2` remains enabled, so normal-user eBPF-based observability still needs an approved wrapper/capability path.
- `perf` is not currently on `PATH`; install/expose the kernel-matched `perf` binary, for example the equivalent of `linux-tools-5.15.0-176-generic`.

Requested controlled eBPF-based command set for ChronoLog profiling:

| Purpose | Tool | Controlled arguments |
|---|---|---|
| Off-CPU time | `offcputime` from BCC | `-p <pid> -f <seconds <= 120>` |
| Scheduler latency | `runqlat` from BCC | `-p <pid> <seconds <= 120>` |
| Futex/lock symptoms | `funclatency` from BCC or bpftrace | target `futex`, `pthread_mutex_lock`, `pthread_mutex_unlock`; `-p <pid>`; duration <= 120s |
| Syscall latency | `syscount` or bpftrace syscall tracepoints | `-p <pid>`; duration <= 120s |
| Block I/O latency | `biolatency` from BCC | no broad tracing beyond duration <= 120s |
| TCP events | `tcplife`, `tcpretrans`, `tcptop` from BCC | duration <= 120s; target profiling nodes only |

Recommended wrapper behavior:

- Only allow the commands above.
- Require a target PID where supported.
- Require a maximum duration no greater than 120 seconds.
- Write outputs under the caller-provided `.agent/results/<timestamp>/chronolog/profiles/ebpf/` directory.
- Run only on `ares-comp-03` through `ares-comp-06`.
