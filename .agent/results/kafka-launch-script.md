# Kafka Launch Script

Status: complete.

Added `.agent/scripts/kafka_common.sh` and `.agent/scripts/kafka_launch.sh`.

The launch wrapper:

- detects `KAFKA_HOME`, an existing Kafka command path, or a project-local Kafka install under `opt/kafka`
- installs Kafka from the Apache archive into `opt/kafka` if no usable baseline distribution is present
- launches a single-node fixed Kafka baseline with ZooKeeper
- writes generated configs, logs, pids, and launch environment metadata under `.agent/results/YYYYMMDD-HHMMSS/`

Validation evidence:

- `.agent/results/20260511-233827/kafka/stdout.log`
- `.agent/results/20260511-233827/kafka/stderr.log`
