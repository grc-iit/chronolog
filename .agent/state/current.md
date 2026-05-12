# Current State

- current task: detect Linux network measurement commands
- commands running: none
- last successful validation: eBPF-based tools detection found `bpftool` module/local binary and documented missing bpftrace/BCC plus permission limits; evidence in `.agent/results/20260511-221505/`
- current blocker: perf runtime events and eBPF-based observability require cluster/admin permission changes for later profiling-output validation
- next intended step: detect required Linux network measurement commands: `iperf3`, `ss`, `nstat`, `sar -n`, and `ethtool`
