# Actionable Phase 0 Profiling Findings

This is still a small Phase 0 workload, so it should guide instrumentation and benchmark design more than final optimization decisions. It now has enough signal to identify first targets and validates the collection paths needed for longer loop-agent runs.

## Current Signals

- TAU semantic duration data shows client `rpc_send` dominates the observed client-side semantic time in the 20-event distributed append run: total observed `rpc_send` time is about 65.5 ms, with a max event around 41.7 ms.
- ChronoLog semantic timing now covers client metadata/RPC, Visor metadata and recording-group notification, Keeper ingest/queue/timeline, Grapher receive/queue/archive/HDF5 write, Player playback/archive-read, and shared storage/RPC serialization regions. Keeper lock labels are present, but they are no longer the only detailed path.
- Grapher semantic timing shows `storage_write` is visible and nontrivial in this small run: about 19.4 ms total observed, with a max around 19.4 ms.
- `perf` is no longer only a kernel-policy item: a repo-local kernel-matched binary was validated with `perf stat` and distributed `perf record` on `ares-comp-[03-04]`. Raw service/client `.perf.data` and text reports are packaged under `raw/perf/`.
- The benchmark matrix runner is implemented and validated for a two-node ChronoLog message-size sweep, producing `data/benchmark/benchmark_matrix_summary.csv` and `figures/benchmark_append_throughput_matrix.png`.
- Human-facing evolution plots are now generated from timestamped `.agent/results`: `chronolog_throughput_over_time.png` for throughput and `tau_semantic_time_over_time.png` for semantic timing changes over iterations.
- gperftools for ChronoKeeper shows samples in `pool_pop_shared`, `sched_run`, `pthread_sigmask`, `epoll_wait`, and formatting/output paths. This is consistent with scheduler/Argobots and synchronization overhead being worth deeper investigation.
- Darshan confirms service/client I/O capture works, but this append workload mostly captures config/log/HDF5-adjacent small I/O rather than a sustained storage benchmark.

## Not Yet Good Enough

- There is no eBPF off-CPU/futex/run-queue dataset yet because eBPF execution remains disabled for normal users.
- The current benchmark workloads are still short. They validate the runner and result path, but longer duration mode, warmup, repetitions, producer/consumer sweeps, batching, burstiness, storage mode, and role placement controls should be used for performance claims.
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
