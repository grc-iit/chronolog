# eBPF Sudo Validation

Validation date: 2026-05-12

## SLURM Access

Direct SSH to `ares-comp-[03-08]` is blocked without an active SLURM job:

```text
Access denied by pam_slurm_adopt: you have no active jobs on this node
```

Validation must run through SLURM allocation or `srun`.

## Node State

At validation time:

```text
ares-comp-03 idle
ares-comp-04 idle
ares-comp-05 idle
ares-comp-06 idle
ares-comp-07 inval
ares-comp-08 idle
```

`ares-comp-07` was not allocatable, so the first validation used `ares-comp-[03-06,08]`.

## Tool Availability

Inside an `srun` allocation on `ares-comp-[03-06,08]`, these tools are present:

```text
/usr/bin/bpftrace
/usr/sbin/offcputime-bpfcc
/usr/sbin/runqlat-bpfcc
```

## Correct Validation Path

The sudoers configuration is visible when using the operational path:

```bash
salloc --partition=debug --nodes=1 --nodelist=ares-comp-03 --time=00:03:00
ssh ares-comp-03 'sudo -n -l'
```

That path reports the expected compute node and groups:

```text
SSH_NODE=ares-comp-03
groups=... hpc_admin sudo_slurm sudo_profiling
```

and the expected passwordless commands:

```text
(root) NOPASSWD: /usr/bin/bpftrace, /usr/bin/perf, /usr/sbin/runqlat-bpfcc, /usr/sbin/offcputime-bpfcc
```

Version validation through the same path:

```text
bpftrace v0.14.0
perf version 5.15.199
```

## Earlier Srun-Only Result

An earlier one-shot `srun` test did not show the `sudo_profiling` group and therefore did not match the sudoers rule:

```text
sudo -n /usr/bin/bpftrace --version
sudo: a password is required

sudo -n /usr/sbin/runqlat-bpfcc -h
sudo: a password is required

sudo -n /usr/sbin/offcputime-bpfcc -h
sudo: a password is required
```

On `ares-comp-03`, `/etc/sudoers.d/sudo_profiling` exists, but `sudo -n -l` still reports:

```text
sudo: a password is required
```

The user identity inside that one-shot `srun` job was:

```text
uid=1008(jcernudagarcia) gid=65534(nogroup) groups=65534(nogroup),135(docker),11014(sudo_slurm)
```

The correct ProfileForge collection path should therefore use the existing ChronoLog launch shape: allocate first, then run/SSH onto allocated compute nodes. Direct SSH without an active allocation is blocked by `pam_slurm_adopt`.

## Admin Follow-Up

ChronoLog should continue to run as the normal user. Only eBPF-based tools need sudo.

Current sudoers works on `ares-comp-03` through the `salloc` plus `ssh` path. Repeat validation is still needed on the remaining enabled nodes.

If another node fails, ensure the sudoers rule matches one of:

```text
jcernudagarcia
%sudo_slurm
```

and uses `NOPASSWD` for the actual installed command paths:

```text
/usr/bin/bpftrace
/usr/sbin/offcputime-bpfcc
/usr/sbin/runqlat-bpfcc
```

The ProfileForge harness now has a nonfatal `--profile-mode ebpf` path. It will collect outputs under:

```text
.agent/results/<run>/chronolog/profiles/ebpf/
```

If sudo is still unavailable, the eBPF collector records the sudo failure in the `.err` files and the benchmark can continue.
