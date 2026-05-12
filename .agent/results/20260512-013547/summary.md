# Kafka Distributed Append Smoke

- system: Kafka
- workflow: range_retrieval
- deployment_mode: bare_metal
- node_count: 2
- zookeeper_node: ares-comp-03
- broker_node: ares-comp-04
- bootstrap_server: 172.25.101.4:33092
- operation_count: 10
- message_size_bytes: 1024
- metrics: kafka/metrics.json
