# Distributed SLURM and Network Evidence

Status: complete for first distributed-node access and network/RDMA evidence.

SLURM two-node smoke:

- Result directory: `.agent/results/20260512-003805/`
- Command: `srun --partition=debug --nodes=2 --ntasks=2 --time=00:02:00 hostname`
- Status: success
- Nodes: `ares-comp-03`, `ares-comp-04`

Network/RDMA capture:

- Result directory: `.agent/results/20260512-003828/`
- Command script: `.agent/results/20260512-003828/config/distributed-network-command.sh`
- Output: `.agent/results/20260512-003828/network/stdout.log`
- Status: success

Observed relevant network configuration:

- Both nodes expose `eno1` on `172.20.101.x/16`, 1 Gb/s, link detected.
- Both nodes expose `enp47s0np0` on `172.25.101.x/16`, 40 Gb/s, link detected.
- RDMA device `mlx5_0` is visible on both nodes.
- `ibv_devinfo` reports active port state and Ethernet link layer, consistent with RoCE-capable hardware exposure.

Implication:

- Distributed SLURM access is available from this session; Phase 0 is not blocked by scheduler access.
- Final Phase 0 still requires running the selected workflows on ChronoLog, Kafka, and Mofka in distributed mode and recording the exact transport/interface selections used by each system run.
