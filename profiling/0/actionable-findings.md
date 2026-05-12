# Actionable Phase 0 Profiling Findings

This is still a small Phase 0 workload, so it should guide instrumentation and benchmark design more than final optimization decisions. It now has enough signal to identify first targets.

## Current Signals

- TAU semantic duration data shows client `rpc_send` dominates the observed client-side semantic time in the 20-event distributed append run: total observed `rpc_send` time is about 62.8 ms, with a max event around 47.9 ms.
- Keeper semantic timing shows `keeper_ingest` is the meaningful service-side region currently visible: about 25-28 ms total observed per keeper profile, with max events around 25-28 ms.
- Grapher semantic timing shows `storage_write` is visible and nontrivial in this small run: about 23.6 ms total observed, with a max around 23.6 ms.
- gperftools for ChronoKeeper shows samples in `pool_pop_shared`, `sched_run`, `pthread_sigmask`, `epoll_wait`, and formatting/output paths. This is consistent with scheduler/Argobots and synchronization overhead being worth deeper investigation.
- Darshan confirms service/client I/O capture works, but this append workload mostly captures config/log/HDF5-adjacent small I/O rather than a sustained storage benchmark.

## Not Yet Good Enough

- TAU labels are not yet fine enough around Keeper locking and ingestion internals.
- There is no eBPF off-CPU/futex/run-queue dataset yet because eBPF execution remains disabled for normal users.
- `perf` kernel policy is fixed on target nodes, but the `perf` binary is not available on `PATH`; tool availability must be fixed next.
- Benchmark workloads are still too small to make performance claims. The benchmark framework needs longer duration mode, warmup, repetitions, producer/consumer sweeps, message-size sweeps, batching, burstiness, storage mode, and role placement controls.

## Next Instrumentation Labels

- `keeper_queue_push`
- `keeper_queue_pop`
- `keeper_queue_wait`
- `keeper_lock_wait`
- `keeper_lock_hold`
- `keeper_ingest_to_buffer`
- `keeper_extract_batch`
- `keeper_batch_size`
- `grapher_hdf5_open`
- `grapher_hdf5_write`
- `grapher_hdf5_flush`
- `client_rpc_wait`
- `client_append_total`
