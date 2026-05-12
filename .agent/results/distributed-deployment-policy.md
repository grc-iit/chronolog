# Distributed Deployment Policy

Status: complete.

Phase 0 final benchmark runs target distributed deployments, not local single-node smoke tests on `ares`.

Policy recorded in `.agent/config/phase0-workflows.md` and `.agent/config/phase0-workflows.json`:

- Local single-node runs on the master/login node are smoke validations only.
- Bare-metal SLURM deployments are preferred.
- Containerized deployment is allowed as a fallback or convenience path if it unblocks measurement plumbing.
- Each run must record the deployment mode.
- The distributed sweep remains 1, 2, 4, and 8 nodes where cluster limits allow.

Reason: later ChronoLog optimization work is expected to depend on correct RDMA/RoCE-capable network configuration.
