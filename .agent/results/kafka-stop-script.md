# Kafka Stop/Cleanup Script

Status: complete.

Added `.agent/scripts/kafka_stop.sh`.

The stop wrapper:

- stops only broker and ZooKeeper pids recorded under a Phase 0 result directory
- appends shutdown evidence to that run's Kafka stdout/stderr logs
- optionally removes Kafka runtime data under the result directory with `--clean-data`

Validation evidence:

- `.agent/results/20260511-233928/kafka/stdout.log`
- `.agent/results/20260511-233928/kafka/stderr.log`
