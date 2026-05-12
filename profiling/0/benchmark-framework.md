# Benchmark Framework Direction

Phase 0 needs small validated smokes, but the optimization loop needs a tunable benchmark framework. Mofka's generator is the right reference because it exposes the dimensions a distributed event/log system actually cares about.

Dimensions to carry forward:

| Dimension | Mofka generator concept | Common harness mapping |
|---|---|---|
| Event count | number of events | `operation_count` |
| Event size | data total size / block size | `message_size_bytes` |
| Producers | producer count and producer threads | writer `client_count` |
| Consumers | consumer count and consumer threads | reader `client_count` when comparable |
| Partitions | topic partitions | Kafka partitions, Mofka partitions, ChronoLog chronicle/story layout until ChronoLog has a partition-equivalent |
| Servers | service process counts | `node_count` and role placement |
| Persistence | metadata/data persistence flags and paths | storage mode plus result config manifest |
| Batching | batch size and adaptive batching | future harness parameter |
| Burstiness | burst size, wait between events/bursts | future harness parameter |
| Flush behavior | flush between bursts / flush every N | future harness parameter |
| Mixed mode | simultaneous producer and consumer | future comparable workflow once ChronoLog live/tail read exists |

Phase 0 fair comparison should stay conservative:

- append throughput
- append latency
- archive-backed range/replay where supported
- 1, 2, 4, 8 node scaling when allocation limits allow

The loop-agent framework should expand from this baseline into pattern exploration: message size sweeps, producer/consumer sweeps, batching, burstiness, persistence modes, role placement, and storage/network configuration sweeps.
