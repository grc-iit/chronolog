# Kafka Distributed Append Smoke

- system: Kafka
- workflow: append_throughput
- deployment_mode: bare_metal
- node_count: 4
- zookeeper_node: ares-comp-03
- broker_nodes: ares-comp-04,ares-comp-05,ares-comp-06
- bootstrap_server: 172.25.101.4:29092,172.25.101.5:29092,172.25.101.6:29092
- operation_count: 100
- message_size_bytes: 1024
- metrics: kafka/metrics.json
