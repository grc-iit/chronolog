# Configuration Characterization Policy

Status: active Phase 0 requirement.

The final Phase 0 target is distributed deployment. Local single-node runs on `ares` are smoke validations only and do not satisfy the final benchmark requirement. Bare-metal SLURM deployment is preferred because ChronoLog and Mofka are HPC systems and the final comparison should be able to account for RDMA/RoCE-capable network configuration. Containerized deployment is allowed only as a fallback or convenience path and must be recorded explicitly.

Every comparable run should capture a configuration manifest under the run's `config/` directory. The manifest must be sufficient to defend why the comparison was reasonable.

Required common fields:

- deployment mode: `bare_metal`, `containerized`, or `local_smoke`
- allocation: node count, task count, CPU binding, hostnames, SLURM job metadata when available
- workload: workflow name, operation count, message/event size, client count, producer/consumer split, duration target
- storage: data path, persistence setting, filesystem/mount point if detectable
- process/thread shape: server processes, client processes, service threads, worker pools, execution streams when applicable
- memory: configured memory limits/caches/pools where exposed, plus node memory summary
- network: selected transport/protocol, interface, NIC speed, driver, offload/drops where `ethtool` allows, and Linux network measurement command outputs
- software: command paths, versions, build variants, relevant environment variables

System-specific configuration surfaces:

- ChronoLog: chrono-visor/keeper/grapher topology, story/chronicle layout, keeper buffering/flush settings, storage path, Thallium/Mercury transport, profiling mode, TAU/perf/gperftools/Darshan settings.
- Kafka: broker count, topic partition count, replication factor, producer acknowledgements, linger/batch/compression settings, JVM heap, log directory, and advertised listener mapping. Kafka remains a fixed baseline; these are recorded, not tuned.
- Mofka: Bedrock provider layout, Mercury protocol/address, metadata database count/type/path, data storage target count/type/path, persistence flags, pool/execution-stream counts, partition count, producer/consumer thread/batch settings, and benchmark-generator configuration when used. Mofka remains a fixed baseline; these are recorded, not optimized.

The final Phase 0 report must explicitly identify which settings were fixed, which were varied by the workflow sweep, and which remained defaults because no comparable or safe user-level setting was available.
