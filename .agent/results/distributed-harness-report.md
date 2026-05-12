# Distributed Harness

Status: initial wrapper complete.

Artifacts:

- `.agent/scripts/phase0_slurm_run.sh`
- `.agent/scripts/phase0_collect_node_config.sh`

Purpose:

- run commands under a SLURM allocation with a Phase 0 result directory
- capture hostnames, command provenance, SLURM parameters, stdout/stderr, and per-node CPU/memory/network/RDMA configuration
- provide a common base for ChronoLog, Kafka, and Mofka distributed benchmark scripts

Validation:

```text
.agent/scripts/phase0_slurm_run.sh \
  --result-dir .agent/results/20260512-004000 \
  --partition debug \
  --nodes 2 \
  --ntasks 2 \
  --time 00:03:00 \
  --job-name phase0-wrapper-smoke \
  -- hostname
```

Validation status: complete.

The validation result directory contains:

- `config/slurm-command.txt`
- `config/slurm-run.env`
- `config/slurm-hosts.txt`
- `config/slurm-node-config.txt`
- `slurm/stdout.log`
- `slurm/stderr.log`
- `slurm/status.env`

Evidence:

- `.agent/results/20260512-004000/slurm/status.env` contains `status=0`
- `.agent/results/20260512-004000/slurm/stdout.log` contains hostnames from two allocated tasks
- `.agent/results/20260512-004000/config/slurm-node-config.txt` captures CPU, memory, network, and RDMA-visible configuration from allocated nodes
