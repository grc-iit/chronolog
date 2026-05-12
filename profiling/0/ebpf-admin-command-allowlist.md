# eBPF-Based Tools Admin Enablement Request

Target nodes:

- `ares-comp-03`
- `ares-comp-04`
- `ares-comp-05`
- `ares-comp-06`
- `ares-comp-07`
- `ares-comp-08`

Current status:

- `kernel.perf_event_paranoid=1` on `ares-comp-03` and `ares-comp-04`.
- `kernel.yama.ptrace_scope=0` on `ares-comp-03` and `ares-comp-04`.
- Kun Feng is extending those two settings to `ares-comp-05` and `ares-comp-06`.
- `bpfcc-tools` has been upgraded on `ares-comp-[03-08]`; `offcputime-bpfcc` and `runqlat-bpfcc` now work on the upgraded nodes.
- `kernel.unprivileged_bpf_disabled=2` may remain enabled, so normal-user eBPF-based observability still needs sudo for the eBPF tool commands or an approved wrapper/capability path.
- `perf` is not currently on `PATH`; install/expose the kernel-matched `perf` binary, for example the equivalent of `linux-tools-5.15.0-176-generic`.
- Validation through `srun` confirms the eBPF tools are installed on `ares-comp-[03-06,08]`, but passwordless sudo is not yet matching `jcernudagarcia`; see `profiling/0/results/ebpf-sudo-validation.md`.

Important privilege model:

- ChronoLog services and benchmark clients should run as the normal ChronoLog user.
- Only the profiling tools that inspect kernel state need elevated privileges.
- Do **not** run ChronoLog binaries as root for profiling. Use sudo only for controlled commands such as `bpftrace`, `offcputime-bpfcc`, `runqlat-bpfcc`, and related eBPF-based tools.

## Preferred Short-Term Admin Changes

For Phase 0 profiling on the four target nodes, the simplest useful setup is:

```bash
for node in ares-comp-03 ares-comp-04 ares-comp-05 ares-comp-06 ares-comp-07 ares-comp-08; do
  ssh "$node" '
    sudo sysctl -w kernel.perf_event_paranoid=1
    sudo sysctl -w kernel.yama.ptrace_scope=0
    sudo sysctl -w kernel.unprivileged_bpf_disabled=0
  '
done
```

If the change should survive reboot on only those profiling nodes:

```bash
sudo tee /etc/sysctl.d/90-chronolog-phase0-profiling.conf >/dev/null <<'EOF'
kernel.perf_event_paranoid=1
kernel.yama.ptrace_scope=0
kernel.unprivileged_bpf_disabled=0
EOF
sudo sysctl --system
```

Install or expose these tools on the same nodes:

```bash
sudo apt-get update
sudo apt-get install -y linux-tools-$(uname -r) linux-tools-common bpftrace bpfcc-tools linux-headers-$(uname -r)
```

On Ubuntu, BCC commands may be installed with a `-bpfcc` suffix. Either form is fine if it is on `PATH`, for example `offcputime` or `offcputime-bpfcc`.

Validation commands for admins or the ChronoLog user:

```bash
sysctl kernel.perf_event_paranoid kernel.yama.ptrace_scope kernel.unprivileged_bpf_disabled
command -v perf
perf stat true
command -v bpftrace
bpftrace -e 'tracepoint:syscalls:sys_enter_nanosleep { @[comm] = count(); } interval:s:1 { exit(); }'
command -v offcputime || command -v offcputime-bpfcc
command -v runqlat || command -v runqlat-bpfcc
```

Expected sysctl output:

```text
kernel.perf_event_paranoid = 1
kernel.yama.ptrace_scope = 0
kernel.unprivileged_bpf_disabled = 0
```

## Constrained Wrapper Alternative

If admins do not want to set `kernel.unprivileged_bpf_disabled=0`, the fallback is sudo access to a root-owned wrapper or a tightly controlled sudoers command list that runs only the profiling tools below for members of a ChronoLog profiling group. This is acceptable for ProfileForge.

Suggested group and directory:

```bash
sudo groupadd -f chronolog-prof
sudo usermod -aG chronolog-prof jcernudagarcia
sudo install -d -o root -g chronolog-prof -m 0750 /opt/chronolog-prof/bin
```

Wrapper policy:

- Allow only the command set listed below.
- Require `--pid` or `-p` for process-targeted commands.
- Require duration <= 120 seconds.
- Allow execution only on `ares-comp-03` through `ares-comp-06`.
- Write output only under `.agent/results/<timestamp>/chronolog/profiles/ebpf/`.
- Log executed command, user, host, PID, duration, and output path to syslog.

Example sudoers entry once a wrapper exists:

```text
%chronolog-prof ALL=(root) NOPASSWD: /opt/chronolog-prof/bin/chronolog-ebpf
```

If the admins prefer direct sudoers entries instead of a wrapper, the command set should be limited to the installed paths on `ares-comp-[03-08]`, for example:

```text
%chronolog-prof ALL=(root) NOPASSWD: /usr/sbin/bpftrace
%chronolog-prof ALL=(root) NOPASSWD: /usr/sbin/offcputime-bpfcc
%chronolog-prof ALL=(root) NOPASSWD: /usr/sbin/runqlat-bpfcc
%chronolog-prof ALL=(root) NOPASSWD: /usr/sbin/funclatency-bpfcc
%chronolog-prof ALL=(root) NOPASSWD: /usr/sbin/biolatency-bpfcc
%chronolog-prof ALL=(root) NOPASSWD: /usr/sbin/tcplife-bpfcc
%chronolog-prof ALL=(root) NOPASSWD: /usr/sbin/tcpretrans-bpfcc
%chronolog-prof ALL=(root) NOPASSWD: /usr/sbin/tcptop-bpfcc
```

The exact paths should be confirmed with `command -v` on the compute nodes. The wrapper remains safer because sudoers cannot easily constrain durations, PIDs, and output paths for arbitrary tool arguments.

## Requested Controlled Command Set

| Purpose | Tool | Controlled arguments |
|---|---|---|
| Off-CPU time | `offcputime` from BCC | `-p <pid> -f <seconds <= 120>` |
| Scheduler latency | `runqlat` from BCC | `-p <pid> <seconds <= 120>` |
| Futex/lock symptoms | `funclatency` from BCC or bpftrace | target `futex`, `pthread_mutex_lock`, `pthread_mutex_unlock`; `-p <pid>`; duration <= 120s |
| Syscall latency | `syscount` or bpftrace syscall tracepoints | `-p <pid>`; duration <= 120s |
| Block I/O latency | `biolatency` from BCC | no broad tracing beyond duration <= 120s |
| TCP events | `tcplife`, `tcpretrans`, `tcptop` from BCC | duration <= 120s; target profiling nodes only |

## Why These Are Needed

- `offcputime`: identifies whether Keeper/Grapher time is blocked off CPU rather than burning CPU.
- `runqlat`: shows scheduler delay, which matters when Argobots, Mercury, and service threads interact.
- `funclatency`/futex tracing: validates the Keeper lock-contention hypothesis and measures lock wait/hold symptoms.
- `syscount`/syscall tracepoints: shows syscall volume and syscall latency outliers.
- `biolatency`: separates HDF5/storage stalls from ChronoLog CPU or lock stalls.
- `tcplife`, `tcpretrans`, `tcptop`: captures TCP behavior when runs are not using RDMA/RoCE yet.

## Phase 0 Collection Targets

The ChronoLog Phase 0 harness will store eBPF-based outputs here:

```text
.agent/results/YYYYMMDD-HHMMSS/chronolog/profiles/ebpf/
```

The required initial captures are:

- Keeper off-CPU profile during append throughput.
- Keeper futex or pthread mutex latency during append throughput.
- Keeper scheduler latency during append throughput.
- Grapher block I/O latency during archive write.
- Node-level TCP retransmit/top connection summary during the same distributed run.
