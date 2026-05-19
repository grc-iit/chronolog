#!/usr/bin/env python3

import argparse
import json
import multiprocessing
import os
import statistics
import sys
import time
from pathlib import Path


def percentile(values, pct):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, round((pct / 100) * (len(ordered) - 1))))
    return ordered[index]


def make_client(config_path):
    import py_chronolog_client

    config = json.loads(Path(config_path).read_text())
    portal = config["chrono_client"]["VisorClientPortalService"]["rpc"]
    query = config["chrono_client"]["ClientQueryService"]["rpc"]
    client_conf = py_chronolog_client.ClientPortalServiceConf(
        portal["protocol_conf"],
        portal["service_ip"],
        int(portal["service_base_port"]),
        int(portal["service_provider_id"]),
    )
    query_conf = py_chronolog_client.ClientQueryServiceConf(
        query["protocol_conf"],
        query["service_ip"],
        int(query["service_base_port"]),
        int(query["service_provider_id"]),
    )
    return py_chronolog_client.Client(client_conf, query_conf)


def write_client_config(base_config_path, output_path, query_port_offset):
    config = json.loads(Path(base_config_path).read_text(encoding="utf-8"))
    query_rpc = config["chrono_client"]["ClientQueryService"]["rpc"]
    query_rpc["service_base_port"] = int(query_rpc["service_base_port"]) + query_port_offset
    Path(output_path).write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")


def wait_for_oldest(pending_writes, completion_latencies_ms):
    if not pending_writes:
        return 0
    future = pending_writes.pop(0)
    started = time.perf_counter()
    timestamp = future.wait()
    completion_latencies_ms.append((time.perf_counter() - started) * 1000)
    return timestamp


def drain_pending(pending_writes, completion_latencies_ms):
    last_timestamp = 0
    while pending_writes:
        last_timestamp = wait_for_oldest(pending_writes, completion_latencies_ms)
        if last_timestamp == 0:
            return 0
    return last_timestamp


def submit_payloads(handle, payloads, producer_outstanding, pending_writes, submit_latencies_ms, completion_latencies_ms):
    if not payloads:
        return 0
    if len(payloads) == 1 and producer_outstanding <= 1:
        started = time.perf_counter()
        timestamp = handle.log_event(payloads[0])
        completion_latencies_ms.append((time.perf_counter() - started) * 1000)
        return timestamp

    started = time.perf_counter()
    if len(payloads) == 1:
        future = handle.log_event_async(payloads[0])
    else:
        future = handle.log_events_async(payloads)
    submit_latencies_ms.append((time.perf_counter() - started) * 1000)

    if producer_outstanding <= 1:
        if not future.valid():
            return 0
        started = time.perf_counter()
        timestamp = future.wait()
        completion_latencies_ms.append((time.perf_counter() - started) * 1000)
        return timestamp
    if not future.valid():
        return 0
    pending_writes.append(future)
    if len(pending_writes) >= producer_outstanding:
        return wait_for_oldest(pending_writes, completion_latencies_ms)
    return 1


def writer_process(
    writer_id,
    config_path,
    chronicle,
    story,
    operation_count,
    message_size_bytes,
    producer_outstanding,
    producer_batch_size,
    ready_path,
    done_path,
    result_path,
    writer_release_story,
    release_signal_path,
):
    client = make_client(config_path)
    prefix = f"writer={writer_id}:"
    payload = prefix + ("m" * max(0, message_size_bytes - len(prefix)))
    attrs = {}
    flags = 1
    latencies_ms = []
    submit_latencies_ms = []
    progress = {
        "writer_id": writer_id,
        "append_count": 0,
        "append_latencies_ms": latencies_ms,
        "append_latency_semantics": "completion_wait_duration_ms",
        "append_submit_latencies_ms": submit_latencies_ms,
        "producer_outstanding": producer_outstanding,
        "producer_batch_size": producer_batch_size,
    }
    try:
        ret = client.Connect()
        if ret != 0:
            raise RuntimeError(f"writer {writer_id} Connect returned {ret}")
        ret = client.CreateChronicle(chronicle, attrs, flags)
        if ret not in (0, -6):
            raise RuntimeError(f"writer {writer_id} CreateChronicle returned {ret}")
        ret, handle = client.AcquireStory(chronicle, story, attrs, flags)
        if ret != 0 or handle is None:
            raise RuntimeError(f"writer {writer_id} AcquireStory returned {ret}")
        Path(ready_path).write_text(
            json.dumps({"ready": True, "writer_id": writer_id, "time": time.time()}) + "\n",
            encoding="utf-8",
        )
        pending_writes = []
        pending_payloads = []
        for index in range(operation_count):
            pending_payloads.append(f"{writer_id}:{index}:{payload}")
            if len(pending_payloads) < producer_batch_size:
                continue
            timestamp = submit_payloads(
                handle,
                pending_payloads,
                producer_outstanding,
                pending_writes,
                submit_latencies_ms,
                latencies_ms,
            )
            pending_payloads = []
            if timestamp == 0:
                raise RuntimeError(f"writer {writer_id} log_event failed at index {index}")
            progress["append_count"] = index + 1
            if (index + 1) % 1000 == 0:
                Path(result_path).write_text(json.dumps(progress, indent=2) + "\n", encoding="utf-8")
        timestamp = submit_payloads(
            handle,
            pending_payloads,
            producer_outstanding,
            pending_writes,
            submit_latencies_ms,
            latencies_ms,
        )
        if pending_payloads and timestamp == 0:
            raise RuntimeError(f"writer {writer_id} log_event failed while flushing final batch")
        progress["append_count"] = operation_count
        had_pending_writes = bool(pending_writes)
        timestamp = drain_pending(pending_writes, latencies_ms)
        if had_pending_writes and timestamp == 0:
            raise RuntimeError(f"writer {writer_id} log_event failed while draining pending writes")
        def release_story():
            release_result = client.ReleaseStory(chronicle, story)
            progress["release_ret"] = release_result
            if release_result != 0:
                raise RuntimeError(f"writer {writer_id} ReleaseStory returned {release_result}")

        if writer_release_story == "after_reader":
            Path(done_path).write_text(
                json.dumps({"done": True, "writer_id": writer_id, "time": time.time()}) + "\n",
                encoding="utf-8",
            )
            release_signal = Path(release_signal_path)
            release_deadline = time.monotonic() + 300
            while not release_signal.exists():
                if time.monotonic() > release_deadline:
                    raise RuntimeError(f"writer {writer_id} timed out waiting for reader release signal")
                time.sleep(0.05)
            release_story()
        elif writer_release_story == "yes":
            release_story()
            Path(done_path).write_text(
                json.dumps({"done": True, "writer_id": writer_id, "time": time.time()}) + "\n",
                encoding="utf-8",
            )
        else:
            progress["release_ret"] = None
            progress["release_skipped"] = True
            Path(done_path).write_text(
                json.dumps({"done": True, "writer_id": writer_id, "time": time.time()}) + "\n",
                encoding="utf-8",
            )
        progress["success"] = True
    except Exception as exc:
        progress["success"] = False
        progress["error"] = str(exc)
        Path(done_path).write_text(
            json.dumps({"done": True, "writer_id": writer_id, "error": str(exc), "time": time.time()}) + "\n",
            encoding="utf-8",
        )
        raise
    finally:
        Path(result_path).write_text(json.dumps(progress, indent=2) + "\n", encoding="utf-8")
        try:
            client.Disconnect()
        except Exception:
            pass


def reader_process(
    config_path,
    chronicle,
    story,
    expected_event_count,
    ready_paths,
    done_paths,
    result_path,
    interval_ms,
    tail_read_mode,
    tail_overlap_ns,
    final_deadline_seconds,
):
    import py_chronolog_client

    client = make_client(config_path)
    attrs = {}
    flags = 1
    latencies_ms = []
    retrieved_counts = []
    incremental_counts = []
    result = {
        "tail_read_count": 0,
        "tail_success_count": 0,
        "tail_read_mode": tail_read_mode,
        "tail_overlap_ns": tail_overlap_ns,
        "tail_latencies_ms": latencies_ms,
        "retrieved_counts": retrieved_counts,
        "incremental_retrieved_counts": incremental_counts,
        "reader_duration_seconds": 0.0,
    }
    reader_started = time.perf_counter()
    try:
        ready_files = [Path(path) for path in ready_paths]
        done_files = [Path(path) for path in done_paths]
        deadline = time.monotonic() + 120
        while not all(path.exists() for path in ready_files):
            if time.monotonic() > deadline:
                raise RuntimeError("timed out waiting for writer ready files")
            time.sleep(0.01)
        ret = client.Connect()
        if ret != 0:
            raise RuntimeError(f"reader Connect returned {ret}")
        ret, handle = client.AcquireStory(chronicle, story, attrs, flags)
        if ret != 0 or handle is None:
            raise RuntimeError(f"reader AcquireStory returned {ret}")

        interval = max(1, interval_ms) / 1000.0
        next_start_time = 1
        seen_incremental_events = set()
        total_keeper_cursor_events = 0
        writer_done_at = None
        packed_events = py_chronolog_client.PackedReplayBatch() if tail_read_mode == "keeper_cursor_packed" else None
        while True:
            if writer_done_at is None and all(path.exists() for path in done_files):
                writer_done_at = time.monotonic()
            events = py_chronolog_client.EventList()
            started = time.perf_counter()
            if tail_read_mode == "keeper_cursor_packed":
                ret = handle.replay_tail_incremental_packed(2000000000000000000, packed_events)
            elif tail_read_mode == "keeper_cursor":
                ret = handle.replay_tail_incremental(2000000000000000000, events)
            else:
                ret = client.ReplayStory(chronicle, story, next_start_time, 2000000000000000000, events)
            elapsed_ms = (time.perf_counter() - started) * 1000
            latencies_ms.append(elapsed_ms)
            result["tail_read_count"] += 1
            if ret == 0:
                result["tail_success_count"] += 1
                event_count = len(events)
                if tail_read_mode == "keeper_cursor_packed":
                    event_count = packed_events.event_count()
                    total_keeper_cursor_events += event_count
                    incremental_counts.append(event_count)
                    retrieved_counts.append(total_keeper_cursor_events)
                    result["packed_tail_payload_bytes"] = (
                        int(result.get("packed_tail_payload_bytes") or 0) + int(packed_events.payload_bytes())
                    )
                elif tail_read_mode == "keeper_cursor":
                    total_keeper_cursor_events += event_count
                    incremental_counts.append(event_count)
                    retrieved_counts.append(total_keeper_cursor_events)
                elif tail_read_mode == "incremental":
                    new_event_count = 0
                    if event_count > 0:
                        max_event_time = next_start_time
                        for event in events:
                            event_key = (event.time(), event.client_id(), event.index())
                            max_event_time = max(max_event_time, event.time())
                            if event_key in seen_incremental_events:
                                continue
                            seen_incremental_events.add(event_key)
                            new_event_count += 1
                        # Replay with a bounded overlap because a single global timestamp
                        # watermark can outrun late-visible events from other Keepers.
                        # Composite de-duplication handles repeated returns.
                        next_start_time = max(1, max_event_time - tail_overlap_ns)
                    incremental_counts.append(new_event_count)
                    retrieved_counts.append(len(seen_incremental_events))
                else:
                    retrieved_counts.append(event_count)
            if all(path.exists() for path in done_files) and retrieved_counts and max(retrieved_counts) >= expected_event_count:
                break
            if writer_done_at is not None and time.monotonic() - writer_done_at > final_deadline_seconds:
                break
            if len(latencies_ms) % 10 == 0:
                Path(result_path).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
            time.sleep(interval)
        result["success"] = bool(retrieved_counts and max(retrieved_counts) >= expected_event_count)
    except Exception as exc:
        result["success"] = False
        result["error"] = str(exc)
        raise
    finally:
        result["reader_duration_seconds"] = time.perf_counter() - reader_started
        Path(result_path).write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        try:
            client.ReleaseStory(chronicle, story)
        except Exception:
            pass
        try:
            client.Disconnect()
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser(description="Run a mixed ChronoLog append plus live tail benchmark.")
    parser.add_argument("--config", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--operation-count", type=int, default=10000)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--node-count", type=int, default=4)
    parser.add_argument("--client-count", type=int, default=1)
    parser.add_argument("--tail-interval-ms", type=int, default=10)
    parser.add_argument(
        "--tail-read-mode",
        choices=["full", "incremental", "keeper_cursor", "keeper_cursor_packed"],
        default="full",
    )
    parser.add_argument("--tail-reader-start-mode", choices=["ready", "after_writers_done"], default="ready")
    parser.add_argument("--tail-overlap-ns", type=int, default=1_000_000_000)
    parser.add_argument("--tail-final-deadline-seconds", type=int, default=60)
    parser.add_argument("--reader-join-timeout-seconds", type=int, default=180)
    parser.add_argument("--producer-outstanding", type=int, default=1)
    parser.add_argument("--producer-batch-size", type=int, default=1)
    parser.add_argument(
        "--writer-release-story",
        choices=["yes", "no", "after_reader"],
        default=os.environ.get("CHRONOLOG_MIXED_TAIL_WRITER_RELEASE_STORY", "yes"),
    )
    args = parser.parse_args()
    if args.client_count < 1:
        raise SystemExit("chronolog mixed_append_tail requires client-count >= 1")
    if args.producer_outstanding < 1:
        raise SystemExit("--producer-outstanding must be >= 1")
    if args.producer_batch_size < 1:
        raise SystemExit("--producer-batch-size must be >= 1")

    result_root = Path(args.result_dir)
    chronolog_dir = result_root / "chronolog"
    chronolog_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"{int(time.time() * 1000)}_{os.getpid()}"
    chronicle = f"phase0_mixed_chronicle_{suffix}"
    story = f"phase0_mixed_story_{suffix}"
    ready_paths = [chronolog_dir / f"mixed-writer-{writer_id}-ready.json" for writer_id in range(args.client_count)]
    done_paths = [chronolog_dir / f"mixed-writer-{writer_id}-done.json" for writer_id in range(args.client_count)]
    writer_result_paths = [chronolog_dir / f"mixed-writer-{writer_id}.json" for writer_id in range(args.client_count)]
    reader_result_path = chronolog_dir / "mixed-reader.json"
    release_signal_path = chronolog_dir / "mixed-reader-finished.release"
    writer_config_paths = [
        chronolog_dir / f"mixed-writer-{writer_id}-client-conf.json" for writer_id in range(args.client_count)
    ]
    reader_config_path = chronolog_dir / "mixed-reader-client-conf.json"
    release_signal_path.unlink(missing_ok=True)
    for writer_id, writer_config_path in enumerate(writer_config_paths):
        write_client_config(args.config, writer_config_path, writer_id)
    write_client_config(args.config, reader_config_path, args.client_count)

    started = time.perf_counter()
    writers = []
    for writer_id in range(args.client_count):
        writers.append(
            multiprocessing.Process(
                target=writer_process,
                args=(
                    writer_id,
                    str(writer_config_paths[writer_id]),
                    chronicle,
                    story,
                    args.operation_count,
                    args.message_size_bytes,
                    args.producer_outstanding,
                    args.producer_batch_size,
                    str(ready_paths[writer_id]),
                    str(done_paths[writer_id]),
                    str(writer_result_paths[writer_id]),
                    args.writer_release_story,
                    str(release_signal_path),
                ),
            )
        )
    expected_event_count = args.operation_count * args.client_count
    reader = multiprocessing.Process(
        target=reader_process,
        args=(
            str(reader_config_path),
            chronicle,
            story,
            expected_event_count,
            [str(path) for path in ready_paths],
            [str(path) for path in done_paths],
            str(reader_result_path),
            args.tail_interval_ms,
            args.tail_read_mode,
            args.tail_overlap_ns,
            args.tail_final_deadline_seconds,
        ),
    )
    append_started = time.perf_counter()
    for writer in writers:
        writer.start()
    if args.tail_reader_start_mode == "ready":
        reader.start()
    if args.writer_release_story == "after_reader":
        done_deadline = time.monotonic() + 600
        while not all(path.exists() for path in done_paths):
            if time.monotonic() > done_deadline:
                break
            if any(not writer.is_alive() and not done_paths[index].exists() for index, writer in enumerate(writers)):
                break
            time.sleep(0.05)
        append_duration = time.perf_counter() - append_started
        if args.tail_reader_start_mode == "after_writers_done":
            reader.start()
    else:
        for writer in writers:
            writer.join(timeout=600)
        for writer in writers:
            if writer.is_alive():
                writer.terminate()
                writer.join(timeout=10)
        append_duration = time.perf_counter() - append_started
        if args.tail_reader_start_mode == "after_writers_done":
            reader.start()
    reader.join(timeout=args.reader_join_timeout_seconds)
    if reader.is_alive():
        reader.terminate()
        reader.join(timeout=10)
    release_signal_path.write_text(json.dumps({"reader_done": True, "time": time.time()}) + "\n", encoding="utf-8")
    for writer in writers:
        writer.join(timeout=60)
    for writer in writers:
        if writer.is_alive():
            writer.terminate()
            writer.join(timeout=10)
    duration = time.perf_counter() - started

    writer_results = [
        json.loads(path.read_text(encoding="utf-8")) if path.exists() else {} for path in writer_result_paths
    ]
    reader_result = json.loads(reader_result_path.read_text(encoding="utf-8")) if reader_result_path.exists() else {}
    append_latencies = [
        latency for writer_result in writer_results for latency in writer_result.get("append_latencies_ms", [])
    ]
    append_submit_latencies = [
        latency for writer_result in writer_results for latency in writer_result.get("append_submit_latencies_ms", [])
    ]
    tail_latencies = reader_result.get("tail_latencies_ms", [])
    retrieved_counts = reader_result.get("retrieved_counts", [])
    append_count = sum(int(writer_result.get("append_count") or 0) for writer_result in writer_results)
    max_retrieved = max(retrieved_counts) if retrieved_counts else 0
    tail_reader_duration = float(reader_result.get("reader_duration_seconds") or 0.0)
    tail_latency_total_seconds = sum(float(value) for value in tail_latencies) / 1000.0
    writer_exitcodes = [writer.exitcode for writer in writers]
    success = (
        all(exitcode == 0 for exitcode in writer_exitcodes)
        and reader.exitcode == 0
        and append_count >= expected_event_count
        and max_retrieved >= expected_event_count
    )
    metrics = {
        "system": "chronolog",
        "workflow": "mixed_append_tail",
        "node_count": args.node_count,
        "nodes": args.node_count,
        "client_count": args.client_count,
        "parallel_clients": args.client_count,
        "message_size_bytes": args.message_size_bytes,
        "operation_count": args.operation_count,
        "operation_count_per_client": args.operation_count,
        "message_count_per_client": args.operation_count,
        "messages_per_client": args.operation_count,
        "total_operation_count": expected_event_count,
        "total_message_count": expected_event_count,
        "total_messages": expected_event_count,
        "total_payload_bytes": expected_event_count * args.message_size_bytes,
        "parallel_client_count": args.client_count,
        "chronolog_client_batch_keeper_selection": os.environ.get(
            "CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION", "per_event"
        ),
        "duration_seconds": duration,
        "throughput_ops_per_sec": append_count / duration if duration > 0 else 0.0,
        "workflow_duration_seconds": duration,
        "workflow_throughput_ops_per_sec": append_count / duration if duration > 0 else 0.0,
        "append_wall_duration_seconds": append_duration,
        "append_wall_throughput_ops_per_sec": append_count / append_duration if append_duration > 0 else 0.0,
        "avg_latency_ms": statistics.fmean(append_latencies) if append_latencies else None,
        "p50_latency_ms": percentile(append_latencies, 50),
        "p95_latency_ms": percentile(append_latencies, 95),
        "p99_latency_ms": percentile(append_latencies, 99),
        "append_latency_semantics": "completion_wait_duration_ms",
        "append_submit_avg_latency_ms": statistics.fmean(append_submit_latencies) if append_submit_latencies else None,
        "append_submit_p50_latency_ms": percentile(append_submit_latencies, 50),
        "append_submit_p95_latency_ms": percentile(append_submit_latencies, 95),
        "append_submit_p99_latency_ms": percentile(append_submit_latencies, 99),
        "success": success,
        "append_count": append_count,
        "writer_success_count": sum(1 for writer_result in writer_results if writer_result.get("success")),
        "writer_exitcodes": writer_exitcodes,
        "tail_read_count": int(reader_result.get("tail_read_count") or 0),
        "tail_success_count": int(reader_result.get("tail_success_count") or 0),
        "tail_max_retrieved_count": max_retrieved,
        "tail_final_retrieved_count": retrieved_counts[-1] if retrieved_counts else 0,
        "packed_tail_payload_bytes": int(reader_result.get("packed_tail_payload_bytes") or 0),
        "tail_avg_latency_ms": statistics.fmean(tail_latencies) if tail_latencies else None,
        "tail_p50_latency_ms": percentile(tail_latencies, 50),
        "tail_p95_latency_ms": percentile(tail_latencies, 95),
        "tail_p99_latency_ms": percentile(tail_latencies, 99),
        "tail_reader_duration_seconds": tail_reader_duration,
        "tail_latency_total_seconds": tail_latency_total_seconds,
        "tail_retrieval_throughput_ops_per_sec": (
            max_retrieved / tail_reader_duration if tail_reader_duration > 0 else 0.0
        ),
        "tail_rpc_active_throughput_ops_per_sec": (
            max_retrieved / tail_latency_total_seconds if tail_latency_total_seconds > 0 else 0.0
        ),
        "tail_interval_ms": args.tail_interval_ms,
        "tail_read_mode": args.tail_read_mode,
        "tail_reader_start_mode": args.tail_reader_start_mode,
        "tail_overlap_ns": args.tail_overlap_ns,
        "tail_final_deadline_seconds": args.tail_final_deadline_seconds,
        "reader_join_timeout_seconds": args.reader_join_timeout_seconds,
        "chronolog_producer_outstanding": args.producer_outstanding,
        "chronolog_producer_batch_size": args.producer_batch_size,
        "producer_wait_boundary": (
            "per_event_wait"
            if args.producer_outstanding <= 1 and args.producer_batch_size <= 1
            else f"bounded_outstanding_wait_after_{args.producer_outstanding}_calls_batch_{args.producer_batch_size}"
        ),
        "tail_incremental_retrieved_events": max_retrieved if args.tail_read_mode in {"incremental", "keeper_cursor"} else None,
        "live_tail_attempted": True,
        "live_tail_succeeded": max_retrieved >= expected_event_count,
        "writer_exitcode": 0 if all(exitcode == 0 for exitcode in writer_exitcodes) else 1,
        "reader_exitcode": reader.exitcode,
    }
    (chronolog_dir / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metrics, indent=2))
    if not success:
        raise SystemExit(1)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"chronolog mixed append/tail benchmark failed: {exc}", file=sys.stderr)
        raise
