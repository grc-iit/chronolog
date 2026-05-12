# Current State

- current task: create Mofka launch script
- commands running: none
- last successful validation: distributed deployment policy was recorded; single-node `ares` runs are now explicitly smoke-only and bare-metal SLURM deployment is preferred, with evidence in `.agent/config/phase0-workflows.md` and `.agent/config/phase0-workflows.json`
- current blocker: perf runtime events and eBPF-based observability require cluster/admin permission changes for later profiling-output validation
- next intended step: locate Mofka installation or available modules, then add a bare-metal-first Mofka fixed-baseline launch wrapper without modifying Mofka source
