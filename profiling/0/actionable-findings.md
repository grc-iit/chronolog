# Actionable Phase 0 Profiling Findings

This is still a small Phase 0 workload, so it should guide instrumentation and benchmark design more than final optimization decisions. It now has enough signal to identify first targets and validates the collection paths needed for longer loop-agent runs.

## Current Signals

- TAU semantic duration data shows client `rpc_send` dominates the observed client-side semantic time in the 20-event distributed append run: total observed `rpc_send` time is about 65.5 ms, with a max event around 41.7 ms.
- Keeper semantic timing now separates datastore lock wait/hold, ingestion-queue lock wait/hold, queue swap/push, timeline lock wait/hold, and merge events. In the validated 20-event run, broad `keeper_ingest` remains the largest Keeper bucket, while the new lock labels are visible and ready for larger contention runs.
- Grapher semantic timing shows `storage_write` is visible and nontrivial in this small run: about 19.4 ms total observed, with a max around 19.4 ms.
- `perf` is no longer only a kernel-policy item: a repo-local kernel-matched binary was validated with `perf stat` and distributed `perf record` on `ares-comp-[03-04]`. Raw service/client `.perf.data` and text reports are packaged under `raw/perf/`.
- The benchmark matrix runner is implemented and validated for a two-node ChronoLog message-size sweep, producing `data/benchmark/benchmark_matrix_summary.csv` and `figures/benchmark_append_throughput_matrix.png`.
- gperftools for ChronoKeeper shows samples in `pool_pop_shared`, `sched_run`, `pthread_sigmask`, `epoll_wait`, and formatting/output paths. This is consistent with scheduler/Argobots and synchronization overhead being worth deeper investigation.
- Darshan confirms service/client I/O capture works, but this append workload mostly captures config/log/HDF5-adjacent small I/O rather than a sustained storage benchmark.

## Not Yet Good Enough

- There is no eBPF off-CPU/futex/run-queue dataset yet because eBPF execution remains disabled for normal users.
- The current benchmark workloads are still intentionally small. They validate the runner and result path, but longer duration mode, warmup, repetitions, producer/consumer sweeps, batching, burstiness, storage mode, and role placement controls should be used for performance claims.
- TAU trace-mode timeline generation through Jumpshot is not yet validated; current TAU evidence is profile-mode user-event timing.

## Next Instrumentation Labels

- `keeper_queue_push`
- `keeper_queue_pop`
- `keeper_queue_wait`
- `keeper_ingest_to_buffer`
- `keeper_extract_batch`
- `keeper_batch_size`
- `grapher_hdf5_open`
- `grapher_hdf5_write`
- `grapher_hdf5_flush`
- `client_rpc_wait`
- `client_append_total`
