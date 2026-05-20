#!/usr/bin/env python3

import argparse
import concurrent.futures
import json
import os
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path


def percentile(values, percent):
    if not values:
        return None
    ordered = sorted(values)
    index = int(round((percent / 100.0) * (len(ordered) - 1)))
    return ordered[index]


def group_ranks(group_file):
    try:
        members = json.loads(Path(group_file).read_text()).get("members", [])
    except Exception:
        return []
    return list(range(len(members)))


def group_member_address(group_file, rank):
    try:
        members = json.loads(Path(group_file).read_text()).get("members", [])
    except Exception as exc:
        raise RuntimeError(f"cannot read Mofka group file {group_file}: {exc}") from exc
    if rank < 0 or rank >= len(members):
        raise RuntimeError(f"Mofka rank {rank} is not present in group file with {len(members)} members")
    address = members[rank].get("address")
    if not address:
        raise RuntimeError(f"Mofka rank {rank} has no address in group file")
    return address


def precreated_provider_locator(group_file, rank, provider_name):
    return f"{provider_name}@{group_member_address(group_file, rank)}"


def main():
    parser = argparse.ArgumentParser(description="Run a Phase 0 Mofka append/read benchmark.")
    parser.add_argument("--group-file", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--topic", default="phase0_append")
    parser.add_argument("--operation-count", type=int, default=50)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--node-count", type=int, default=1)
    parser.add_argument("--client-count", type=int, default=1)
    parser.add_argument("--deployment-mode", default="local_validation")
    parser.add_argument("--workflow", default="append_throughput")
    parser.add_argument("--partition-type", choices=["memory", "default"], default="memory")
    parser.add_argument("--partition-server-rank", type=int, default=1)
    parser.add_argument("--metadata-provider", default="")
    parser.add_argument("--data-provider", default="")
    parser.add_argument("--storage-target-type", choices=["memory", "abtio", "pmdk"], default="memory")
    parser.add_argument("--storage-path-root", default="")
    parser.add_argument("--storage-target-size", type=int, default=67108864)
    parser.add_argument("--precreate-storage-provider", choices=["yes", "no"], default="yes")
    parser.add_argument("--group-ping-timeout-ms", type=int, default=1000)
    parser.add_argument("--group-ping-interval-min-ms", type=int, default=1000)
    parser.add_argument("--group-ping-interval-max-ms", type=int, default=1000)
    parser.add_argument("--group-ping-max-timeouts", type=int, default=3)
    parser.add_argument("--producer-wait-mode", choices=["per_event", "after_loop", "none"], default="per_event")
    parser.add_argument("--producer-flush-mode", choices=["after_loop", "none"], default="after_loop")
    parser.add_argument("--client-execution-mode", choices=["threads", "processes"], default="threads")
    parser.add_argument("--single-client-index", type=int, default=None)
    parser.add_argument("--skip-topic-setup", action="store_true")
    parser.add_argument("--client-metrics-file", default="")
    args = parser.parse_args()
    if args.workflow == "range_retrieval" and args.producer_wait_mode == "none":
        raise SystemExit("range_retrieval requires producer-wait-mode per_event or after_loop")

    from mochi.mofka.client import MofkaDriver, Ordering

    result_dir = Path(args.result_dir)
    mofka_dir = result_dir / "mofka"
    config_dir = result_dir / "config"
    mofka_dir.mkdir(parents=True, exist_ok=True)
    config_dir.mkdir(parents=True, exist_ok=True)

    service = MofkaDriver(args.group_file, use_progress_thread=True)
    if not args.skip_topic_setup:
        if not service.topic_exists(args.topic):
            service.create_topic(args.topic)

        if args.partition_type == "memory":
            service.add_memory_partition(args.topic, args.partition_server_rank, pool_name="__primary__")
        elif args.partition_type == "default":
            candidate_ranks = [args.partition_server_rank]
            candidate_ranks.extend(rank for rank in group_ranks(args.group_file) if rank not in candidate_ranks)
            last_error = None
            for rank in candidate_ranks:
                try:
                    metadata_provider = args.metadata_provider
                    data_provider = args.data_provider
                    if args.precreate_storage_provider == "yes":
                        if not metadata_provider:
                            metadata_provider = precreated_provider_locator(
                                args.group_file, rank, "phase0_metadata_provider"
                            )
                        if not data_provider:
                            data_provider = precreated_provider_locator(
                                args.group_file, rank, "phase0_data_provider"
                            )
                    if args.precreate_storage_provider != "yes" and not metadata_provider:
                        metadata_provider = service.add_default_metadata_provider(rank)
                    if args.precreate_storage_provider != "yes" and not data_provider:
                        warabi_target_type = args.storage_target_type
                        target_config = {}
                        if args.storage_target_type == "abtio":
                            storage_root = Path(args.storage_path_root or (mofka_dir / "storage-targets"))
                            storage_root.mkdir(parents=True, exist_ok=True)
                            target_path = storage_root / f"dynamic-data-rank-{rank}.abtio"
                            target_config = {
                                "path": str(target_path),
                                "create_if_missing": True,
                                "override_if_exists": True,
                            }
                        elif args.storage_target_type == "pmdk":
                            storage_root = Path(args.storage_path_root or (mofka_dir / "storage-targets"))
                            storage_root.mkdir(parents=True, exist_ok=True)
                            target_config = {
                                "path": str(storage_root / f"dynamic-data-rank-{rank}.pool"),
                                "create_if_missing_with_size": args.storage_target_size,
                                "override_if_exists": True,
                            }
                        data_provider = service.add_data_provider(
                            rank,
                            target_type=warabi_target_type,
                            target_config=target_config,
                        )
                    service.add_default_partition(
                        args.topic,
                        rank,
                        metadata_provider=metadata_provider,
                        data_provider=data_provider,
                        pool_name="__primary__",
                    )
                    args.partition_server_rank = rank
                    args.metadata_provider = metadata_provider
                    args.data_provider = data_provider
                    break
                except Exception as exc:
                    last_error = exc
            else:
                raise last_error

    topic = service.open_topic(args.topic)

    payload = b"x" * args.message_size_bytes
    print_lock = threading.Lock()

    def append_client(client_index):
        producer = topic.producer(
            f"phase0_producer_{client_index}",
            batch_size=1,
            max_batch=2,
            ordering=Ordering.Loose,
        )
        append_latencies_ms = []
        submit_latencies_ms = []
        wait_latencies_ms = []
        pending_events = []
        client_start = time.perf_counter()
        for index in range(args.operation_count):
            metadata = {"client": client_index, "index": index}
            op_start = time.perf_counter()
            future = producer.push(metadata, payload, partition=0)
            op_end = time.perf_counter()
            submit_latencies_ms.append((op_end - op_start) * 1000.0)
            if args.producer_wait_mode == "per_event":
                wait_start = time.perf_counter()
                event_id = future.wait()
                wait_end = time.perf_counter()
                append_latencies_ms.append((wait_end - op_start) * 1000.0)
                wait_latencies_ms.append((wait_end - wait_start) * 1000.0)
                with print_lock:
                    print(f"client={client_index} event_index={index} event_id={event_id}", flush=True)
            elif args.producer_wait_mode == "after_loop":
                pending_events.append((index, future))
                with print_lock:
                    print(f"client={client_index} event_index={index} event_id=pending", flush=True)
            else:
                with print_lock:
                    print(f"client={client_index} event_index={index} event_id=not_waited", flush=True)
        if args.producer_wait_mode == "after_loop":
            for index, future in pending_events:
                wait_start = time.perf_counter()
                event_id = future.wait()
                wait_end = time.perf_counter()
                wait_latencies_ms.append((wait_end - wait_start) * 1000.0)
                with print_lock:
                    print(f"client={client_index} event_index={index} event_id={event_id}", flush=True)
        if args.producer_flush_mode == "after_loop":
            producer.flush()
        client_end = time.perf_counter()
        return {
            "client_index": client_index,
            "duration_seconds": client_end - client_start,
            "append_latencies_ms": append_latencies_ms,
            "submit_latencies_ms": submit_latencies_ms,
            "wait_latencies_ms": wait_latencies_ms,
        }

    if args.single_client_index is not None:
        item = append_client(args.single_client_index)
        if args.client_metrics_file:
            Path(args.client_metrics_file).write_text(json.dumps(item, indent=2) + "\n")
        return 0

    append_latencies_ms = []
    submit_latencies_ms = []
    wait_latencies_ms = []
    read_latencies_ms = []
    append_start = time.perf_counter()
    client_metrics = []
    if args.client_execution_mode == "processes":
        process_dir = mofka_dir / "client-processes"
        process_dir.mkdir(parents=True, exist_ok=True)
        procs = []
        for client_index in range(args.client_count):
            metrics_path = process_dir / f"client-{client_index}.json"
            stdout_path = process_dir / f"client-{client_index}.stdout.log"
            stderr_path = process_dir / f"client-{client_index}.stderr.log"
            cmd = [
                sys.executable,
                str(Path(__file__).resolve()),
                "--group-file",
                args.group_file,
                "--result-dir",
                args.result_dir,
                "--topic",
                args.topic,
                "--operation-count",
                str(args.operation_count),
                "--message-size-bytes",
                str(args.message_size_bytes),
                "--node-count",
                str(args.node_count),
                "--client-count",
                "1",
                "--deployment-mode",
                args.deployment_mode,
                "--workflow",
                "append_throughput",
                "--partition-type",
                args.partition_type,
                "--partition-server-rank",
                str(args.partition_server_rank),
                "--storage-target-type",
                args.storage_target_type,
                "--storage-path-root",
                args.storage_path_root,
                "--storage-target-size",
                str(args.storage_target_size),
                "--precreate-storage-provider",
                args.precreate_storage_provider,
                "--group-ping-timeout-ms",
                str(args.group_ping_timeout_ms),
                "--group-ping-interval-min-ms",
                str(args.group_ping_interval_min_ms),
                "--group-ping-interval-max-ms",
                str(args.group_ping_interval_max_ms),
                "--group-ping-max-timeouts",
                str(args.group_ping_max_timeouts),
                "--producer-wait-mode",
                args.producer_wait_mode,
                "--producer-flush-mode",
                args.producer_flush_mode,
                "--client-execution-mode",
                "threads",
                "--single-client-index",
                str(client_index),
                "--skip-topic-setup",
                "--client-metrics-file",
                str(metrics_path),
            ]
            with stdout_path.open("w") as stdout_file, stderr_path.open("w") as stderr_file:
                procs.append(
                    (
                        client_index,
                        metrics_path,
                        subprocess.Popen(cmd, stdout=stdout_file, stderr=stderr_file, env=os.environ.copy()),
                    )
                )
        failures = []
        for client_index, metrics_path, proc in procs:
            rc = proc.wait()
            if rc != 0:
                failures.append((client_index, rc))
            elif metrics_path.exists():
                client_metrics.append(json.loads(metrics_path.read_text()))
            else:
                failures.append((client_index, "missing-metrics"))
        if failures:
            raise RuntimeError(f"Mofka process clients failed: {failures}")
    elif args.client_count == 1:
        client_metrics.append(append_client(0))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.client_count) as executor:
            futures = [executor.submit(append_client, client_index) for client_index in range(args.client_count)]
            for future in concurrent.futures.as_completed(futures):
                client_metrics.append(future.result())
    append_end = time.perf_counter()
    client_metrics.sort(key=lambda item: item["client_index"])
    for item in client_metrics:
        append_latencies_ms.extend(item["append_latencies_ms"])
        submit_latencies_ms.extend(item["submit_latencies_ms"])
        wait_latencies_ms.extend(item["wait_latencies_ms"])

    if args.workflow == "range_retrieval":
        consumer = topic.consumer("phase0_consumer", batch_size=1, max_batch=2)
        read_start = time.perf_counter()
        for index in range(args.operation_count * args.client_count):
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
        measured_latencies = append_latencies_ms if args.producer_wait_mode == "per_event" else []
        duration = append_end - append_start

    total_operation_count = args.operation_count * args.client_count
    throughput = total_operation_count / duration if duration > 0 else 0
    if args.partition_type == "memory":
        storage_backend = "mofka_memory_partition"
        durability_boundary = "memory_partition_not_durable_storage"
    else:
        storage_backend = f"mofka_default_partition_{args.storage_target_type}"
        durability_boundary = f"mofka_default_partition_{args.storage_target_type}_storage_target"
    if args.workflow == "range_retrieval":
        semantic_boundary = (
            f"producer_{args.producer_wait_mode}_wait_flush_{args.producer_flush_mode}_then_consumer_pull_catchup"
        )
        read_path = "mofka_consumer_pull_after_append"
    else:
        semantic_boundary = f"producer_push_wait_{args.producer_wait_mode}_flush_{args.producer_flush_mode}"
        read_path = ""
    metrics = {
        "system": "mofka",
        "workflow": args.workflow,
        "node_count": args.node_count,
        "nodes": args.node_count,
        "client_count": args.client_count,
        "parallel_clients": args.client_count,
        "message_size_bytes": args.message_size_bytes,
        "operation_count": args.operation_count,
        "operation_count_per_client": args.operation_count,
        "message_count_per_client": args.operation_count,
        "messages_per_client": args.operation_count,
        "total_operation_count": total_operation_count,
        "total_message_count": total_operation_count,
        "total_messages": total_operation_count,
        "total_payload_bytes": total_operation_count * args.message_size_bytes,
        "parallel_client_count": args.client_count,
        "duration_seconds": duration,
        "throughput_ops_per_sec": throughput,
        "avg_latency_ms": statistics.fmean(measured_latencies) if measured_latencies else None,
        "p50_latency_ms": percentile(measured_latencies, 50),
        "p95_latency_ms": percentile(measured_latencies, 95),
        "p99_latency_ms": percentile(measured_latencies, 99),
        "success": True,
        "producer_wait_mode": args.producer_wait_mode,
        "producer_flush_mode": args.producer_flush_mode,
        "client_execution_mode": args.client_execution_mode,
        "semantic_boundary": semantic_boundary,
        "append_ack_boundary": f"producer_push_wait_{args.producer_wait_mode}",
        "flush_boundary": args.producer_flush_mode,
        "durability_boundary": durability_boundary,
        "read_path": read_path,
        "storage_backend": storage_backend,
        "semantic_notes": (
            "Mofka memory partition rows are memory/live evidence only and must not be used as storage durability "
            "evidence."
            if args.partition_type == "memory"
            else "Mofka default partition row uses the configured storage target and still requires service-health gates."
        ),
        "partition_type": args.partition_type,
        "storage_target_type": args.storage_target_type,
        "storage_target_size": args.storage_target_size,
        "precreate_storage_provider": args.precreate_storage_provider,
        "group_ping_timeout_ms": args.group_ping_timeout_ms,
        "group_ping_interval_min_ms": args.group_ping_interval_min_ms,
        "group_ping_interval_max_ms": args.group_ping_interval_max_ms,
        "group_ping_max_timeouts": args.group_ping_max_timeouts,
        "partition_server_rank": args.partition_server_rank,
        "metadata_provider": args.metadata_provider,
        "data_provider": args.data_provider,
        "submit_avg_latency_ms": statistics.fmean(submit_latencies_ms) if submit_latencies_ms else None,
        "submit_p99_latency_ms": percentile(submit_latencies_ms, 99),
        "wait_avg_latency_ms": statistics.fmean(wait_latencies_ms) if wait_latencies_ms else None,
        "wait_p99_latency_ms": percentile(wait_latencies_ms, 99),
        "client_metrics": [
            {
                "client_index": item["client_index"],
                "duration_seconds": item["duration_seconds"],
                "submit_avg_latency_ms": statistics.fmean(item["submit_latencies_ms"]) if item["submit_latencies_ms"] else None,
                "wait_avg_latency_ms": statistics.fmean(item["wait_latencies_ms"]) if item["wait_latencies_ms"] else None,
            }
            for item in client_metrics
        ],
    }

    (mofka_dir / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    (config_dir / "mofka-workload.json").write_text(json.dumps({
        "workflow": args.workflow,
        "topic": args.topic,
        "partition_type": args.partition_type,
        "deployment_mode": args.deployment_mode,
        "operation_count": args.operation_count,
        "message_size_bytes": args.message_size_bytes,
        "partition_server_rank": args.partition_server_rank,
        "metadata_provider": args.metadata_provider,
        "data_provider": args.data_provider,
        "storage_target_type": args.storage_target_type,
        "storage_path_root": args.storage_path_root,
        "storage_target_size": args.storage_target_size,
        "precreate_storage_provider": args.precreate_storage_provider,
        "group_ping_timeout_ms": args.group_ping_timeout_ms,
        "group_ping_interval_min_ms": args.group_ping_interval_min_ms,
        "group_ping_interval_max_ms": args.group_ping_interval_max_ms,
        "group_ping_max_timeouts": args.group_ping_max_timeouts,
        "producer_batch_size": 1,
        "producer_max_batch": 2,
        "producer_ordering": "Loose",
        "producer_wait_mode": args.producer_wait_mode,
        "producer_flush_mode": args.producer_flush_mode,
        "client_execution_mode": args.client_execution_mode,
        "configuration_note": (
            "Uses Mofka memory partition."
            if args.partition_type == "memory"
            else "Uses Mofka default partition backed by configured Yokan metadata and Warabi data providers."
        ),
    }, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
