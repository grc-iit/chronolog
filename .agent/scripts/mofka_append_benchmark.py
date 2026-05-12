#!/usr/bin/env python3

import argparse
import json
import statistics
import sys
import time
from pathlib import Path


def percentile(values, percent):
    if not values:
        return None
    ordered = sorted(values)
    index = int(round((percent / 100.0) * (len(ordered) - 1)))
    return ordered[index]


def main():
    parser = argparse.ArgumentParser(description="Run a Phase 0 Mofka append/read smoke benchmark.")
    parser.add_argument("--group-file", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--topic", default="phase0_append")
    parser.add_argument("--operation-count", type=int, default=50)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--node-count", type=int, default=1)
    parser.add_argument("--client-count", type=int, default=1)
    parser.add_argument("--deployment-mode", default="local_smoke")
    parser.add_argument("--workflow", default="append_throughput")
    parser.add_argument("--partition-type", choices=["memory"], default="memory")
    args = parser.parse_args()

    from mochi.mofka.client import MofkaDriver, Ordering

    result_dir = Path(args.result_dir)
    mofka_dir = result_dir / "mofka"
    config_dir = result_dir / "config"
    mofka_dir.mkdir(parents=True, exist_ok=True)
    config_dir.mkdir(parents=True, exist_ok=True)

    service = MofkaDriver(args.group_file, use_progress_thread=True)
    if not service.topic_exists(args.topic):
        service.create_topic(args.topic)

    if args.partition_type == "memory":
        service.add_memory_partition(args.topic, 1, pool_name="__primary__")

    topic = service.open_topic(args.topic)
    producer = topic.producer(
        "phase0_producer",
        batch_size=1,
        max_batch=2,
        ordering=Ordering.Loose,
    )

    payload = b"x" * args.message_size_bytes
    append_latencies_ms = []
    read_latencies_ms = []
    append_start = time.perf_counter()
    for index in range(args.operation_count):
        metadata = {"index": index}
        op_start = time.perf_counter()
        event_id = producer.push(metadata, payload, partition=0).wait()
        op_end = time.perf_counter()
        append_latencies_ms.append((op_end - op_start) * 1000.0)
        print(f"event_index={index} event_id={event_id}", flush=True)
    producer.flush()
    append_end = time.perf_counter()

    if args.workflow == "range_retrieval":
        consumer = topic.consumer("phase0_consumer", batch_size=1, max_batch=2)
        read_start = time.perf_counter()
        for index in range(args.operation_count):
            op_start = time.perf_counter()
            event = consumer.pull().wait()
            op_end = time.perf_counter()
            read_latencies_ms.append((op_end - op_start) * 1000.0)
            print(f"read_index={index} event_id={event.event_id}", flush=True)
            try:
                event.acknowledge()
            except Exception:
                pass
        read_end = time.perf_counter()
        measured_latencies = read_latencies_ms
        duration = read_end - read_start
    else:
        measured_latencies = append_latencies_ms
        duration = append_end - append_start

    throughput = args.operation_count / duration if duration > 0 else 0
    metrics = {
        "system": "mofka",
        "workflow": args.workflow,
        "node_count": args.node_count,
        "client_count": args.client_count,
        "message_size_bytes": args.message_size_bytes,
        "operation_count": args.operation_count,
        "duration_seconds": duration,
        "throughput_ops_per_sec": throughput,
        "avg_latency_ms": statistics.fmean(measured_latencies) if measured_latencies else None,
        "p50_latency_ms": percentile(measured_latencies, 50),
        "p95_latency_ms": percentile(measured_latencies, 95),
        "p99_latency_ms": percentile(measured_latencies, 99),
        "success": True,
    }

    (mofka_dir / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    (config_dir / "mofka-workload.json").write_text(json.dumps({
        "workflow": args.workflow,
        "topic": args.topic,
        "partition_type": args.partition_type,
        "deployment_mode": args.deployment_mode,
        "operation_count": args.operation_count,
        "message_size_bytes": args.message_size_bytes,
        "producer_batch_size": 1,
        "producer_max_batch": 2,
        "producer_ordering": "Loose",
        "configuration_note": "Uses Mofka memory partition because Yokan/Warabi-backed dynamic partition creation still needs configuration validation.",
    }, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
