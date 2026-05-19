# Per-Event Durable Admission Analysis

Timestamp: 2026-05-14 00:22 CDT

## Question

Can ChronoLog recover the producer-batch throughput curve while preserving true per-event producer wait semantics?

## Current Evidence

The explicit producer batch-size sweeps show that batching matters:

- 1KiB, 4 nodes, 4 clients, 10000 total events:
  - batch1/2/4/8/16: 5263.10/8765.04/12387.30/21537.80/32992.40 ops/s
  - durable-publish batch avg: 1.098/1.365/1.959/3.256/6.242
  - fdatasync avg: 376.512/274.769/202.494/121.572/72.243 us
- 64KiB, same deployment:
  - batch1/2/4/8/16: 1374.60/3653.10/4279.96/5618.03/6612.60 ops/s
  - durable-publish batch avg: 1.088/1.360/1.941/3.091/5.206
  - fdatasync avg: 1188.454/635.855/627.929/432.405/365.474 us

The same-semantics Keeper wait-window sweep was negative:

- producer batch size stayed 1 and producer outstanding stayed 1.
- wait0/25/50/100us throughput: 5456.61/4520.40/2448.39/4341.02 ops/s.
- durable-publish batch avg only changed 1.099/1.114/1.130/1.181.
- owner queue wait worsened 115.819/230.431/248.744/278.867 us.

## Interpretation

With strict per-event wait, each client submits the next event only after the previous durable completion returns. That means the Keeper owner only has as much natural batching pressure as simultaneously waiting clients. In the c4 workload, the theoretical natural batch pressure is small, and actual arrival skew keeps most batches near one record. A fixed Keeper wait window preserves the client boundary but adds latency without creating enough additional arrivals to pay for itself.

Therefore, the producer-batch curve should be treated as evidence of what batching could recover, not as a same-semantics optimization.

## Design Consequence

For same-semantics durable append, the next ChronoLog-side work should target single-record durable cost and completion overhead:

- reduce per-record WAL write syscall cost, for example via vectored writes where already supported or a more direct append layout;
- reduce fdatasync cost or make it device-appropriate, which is where the original paper's Keeper-local NVRAM/NVMe design matters;
- reduce completion/callback overhead after durable publication;
- reduce descriptor/tail publication cost only if profiling shows it becomes visible again.

For throughput-oriented semantics, keep `producer_batch_size` and `producer_outstanding` explicit. Those are useful workload knobs, but they change producer wait behavior and cannot be used as same-boundary claims against Kafka or Mofka per-event wait rows.

## Decision

Reject fixed Keeper wait-window tuning for this workload. Use the producer-batch curves as motivation for a deeper Keeper WAL admission/completion design, but do not promote batch-size or wait-window knobs as defaults.
