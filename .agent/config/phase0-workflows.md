# Phase 0 Workflows

Status: provisional.

No selected benchmark/workflow list was found in the current repository state. This suite is therefore a proposed default and must be treated as provisional until confirmed or replaced.

## Deployment Target

The final Phase 0 benchmark target is distributed deployment, not single-node `ares` smoke testing. Local single-node runs on the master/login node are allowed only to validate scripts, launch mechanics, logging, and metrics output.

Preferred deployment mode:

1. Bare metal through SLURM allocations, because ChronoLog optimization work is expected to depend on correct RDMA/RoCE-capable network configuration.
2. Containerized deployment only as a fallback or convenience path when it unblocks Phase 0 measurement plumbing, with the mode recorded in each run's config and report.

The distributed sweep remains 1, 2, 4, and 8 nodes where cluster limits allow. A single-node local smoke result does not satisfy the distributed benchmark requirement.

## Comparable Entity Mapping

| Concept | ChronoLog | Kafka | Mofka |
|---|---|---|---|
| Logical container | chronicle/story | topic | stream |
| Record | event | message | record |
| Append/write | client appends an event to an acquired story | producer sends a message to a topic | producer appends a record to a stream |
| Read/range retrieval | story playback/range retrieval where supported | consumer poll by topic/offset range where supported | consumer/read API range retrieval where supported |

## Provisional Suite

| Workflow | Purpose | Required systems | Notes |
|---|---|---|---|
| `append_throughput` | Sustained append/write throughput for fixed-size records | ChronoLog, Kafka, Mofka | Primary baseline workflow. |
| `append_latency` | Append/write latency distribution for fixed-size records | ChronoLog, Kafka, Mofka | Requires per-operation timing in the client harness. |
| `range_retrieval` | Read/range retrieval over previously appended records | ChronoLog, Kafka, Mofka | Run only when all systems expose a comparable path; otherwise document unsupported status with evidence. |
| `mixed_append_read` | Concurrent append and read/range workload | ChronoLog, Kafka, Mofka | Run only when all systems expose compatible APIs; otherwise document unsupported status with evidence. |
| `scaling_sweep` | Append throughput and latency at 1, 2, 4, and 8 nodes | ChronoLog, Kafka, Mofka | Use only node counts allowed by cluster limits and launch support. |

Default parameters are recorded in `.agent/config/phase0-workflows.json` for later benchmark scripts.
