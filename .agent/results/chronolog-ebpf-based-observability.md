# ChronoLog eBPF-based Observability Validation

Checkpoint: verify eBPF-based measurements if available
Status: complete with limitation
Time: 2026-05-11 23:32 CT

## Evidence

The following eBPF-based frontends were not available on `PATH`:

```text
bpftrace execsnoop opensnoop biolatency offcputime runqlat funclatency profile tcplife tcpconnect
```

Kernel and filesystem state:

```text
/proc/sys/kernel/unprivileged_bpf_disabled = 2
/proc/sys/kernel/perf_event_paranoid = 4
/proc/sys/kernel/kptr_restrict = 1
/sys/fs/bpf = root-owned 1730
/sys/kernel/debug = root-owned 0700
/sys/kernel/tracing = root-owned 0700
```

`bpftool` attempts:

- Module `bpftool feature probe unprivileged` exited `255` with `unprivileged run not supported`.
- Local `bpftool prog show` exited `255` with `Operation not permitted`.

Result directory: `.agent/results/20260511-233224/`.

## Limitation

eBPF-based observability for syscall latency, block I/O latency, off-CPU time, scheduler delay, futex/lock contention, and TCP events is not available in this current unprivileged environment. Continuing this specific validation would require administrator support, additional Linux capabilities, or a different cluster allocation where eBPF-based tools are enabled.
