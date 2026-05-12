# eBPF-based Tools Detection

Status: detected with runtime permission limitations.

Evidence directory: `.agent/results/20260511-221505/`

## Detection

No bpftrace or BCC tools were available on `PATH`:

- `bpftrace`
- `execsnoop`
- `opensnoop`
- `ext4slower`
- `biolatency`
- `offcputime`
- `runqlat`
- `funclatency`
- `profile`
- `tcplife`
- `tcpconnect`

The Python BCC module is not installed.

Available module:

```text
bpftool/7.5.0
```

The locally extracted Linux tools from the perf checkpoint also include:

```text
opt/perf/extract/usr/lib/linux-tools-5.15.0-176/bpftool
```

## Validation

Module-provided `bpftool`:

```text
bpftool v7.5.0
using libbpf v1.5
```

Local Linux tools `bpftool`:

```text
bpftool v5.15.199
```

Environment snippet:

```text
.agent/config/ebpf-env.sh
```

## Permission Limits

Kernel settings and mounts indicate unprivileged eBPF-based observability is restricted:

```text
/proc/sys/kernel/unprivileged_bpf_disabled = 2
/proc/sys/kernel/perf_event_paranoid = 4
/proc/sys/kernel/kptr_restrict = 1
/sys/fs/bpf mode = 700 root:root
/sys/kernel/debug mode = 700 root:root
/sys/kernel/tracing mode = 700 root:root
```

`bpftool feature probe unprivileged` reported:

```text
Error: unprivileged run not supported, recompile bpftool with libcap
```

Later eBPF-based observability validation for syscall latency, block I/O latency, off-CPU time, scheduler delay, futex/lock contention, and TCP events will require a permitted cluster environment, additional Linux capabilities, or administrator support.
