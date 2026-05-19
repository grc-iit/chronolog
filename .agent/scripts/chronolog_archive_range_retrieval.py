#!/usr/bin/env python3

import argparse
import concurrent.futures
import json
import multiprocessing
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

from chronolog_append_durable import (
    archive_event_count_wait_trace_metrics,
    archive_file_stat_metrics,
    control_path_profile_metrics,
    grapher_archive_stage_metrics,
    grapher_archive_stage_times,
    grapher_hdf5_write_profile_metrics,
    grapher_orphan_chunk_metrics,
    hdf5_archived_event_count,
    keeper_to_grapher_drain_metrics,
    log_settle_seconds,
    percentile,
    run_client_append,
    wait_for_control_path_profiles,
    wait_for_archive_event_count,
    wait_for_archive_file,
)


def dataset_event_times(files):
    manifest_times = []
    saw_manifest = False
    for path in files:
        manifest_path = Path(str(path) + ".manifest")
        if not manifest_path.exists():
            continue
        saw_manifest = True
        data = json.loads(manifest_path.read_text(encoding="utf-8"))
        event_times = data.get("event_times") or []
        if not event_times:
            continue
        manifest_times.extend(int(value) for value in event_times)
    if manifest_times:
        return sorted(manifest_times), "archive_manifest:event_times"
    if saw_manifest:
        raise RuntimeError("Archive manifest exists but does not contain event_times")

    try:
        import h5py
    except Exception as exc:
        return h5dump_dataset_event_times(files, f"h5py unavailable: {exc}")

    times = []
    dataset_name = ""
    for path in files:
        with h5py.File(path, "r") as handle:
            for candidate in (
                "story_chunks/data.vlen_bytes",
                "story_chunks/data.meta",
                "story_chunks/data.raw_meta",
                "story_chunks/data.fixed_meta",
            ):
                dataset = handle.get(candidate)
                if dataset is None:
                    continue
                if "eventTime" not in (dataset.dtype.names or ()):
                    continue
                times.extend(int(value) for value in dataset["eventTime"])
                dataset_name = candidate
                break
    if not times:
        return h5dump_dataset_event_times(files, "No eventTime metadata found with h5py")
    return sorted(times), dataset_name


def h5dump_dataset_event_times(files, reason):
    candidates = (
        "/story_chunks/data.raw_meta",
        "/story_chunks/data.meta",
        "/story_chunks/data.fixed_meta",
        "/story_chunks/data.vlen_bytes",
    )
    last_error = reason
    for candidate in candidates:
        times = []
        for path in files:
            proc = subprocess.run(
                ["h5dump", "-d", candidate, str(path)],
                text=True,
                capture_output=True,
                timeout=60,
                check=False,
            )
            if proc.returncode != 0:
                last_error = proc.stderr.strip() or proc.stdout.strip() or f"h5dump failed for {candidate}"
                times = []
                break
            record_values = None
            for line in proc.stdout.splitlines():
                if re.match(r"\s*\(\d+\):\s*\{", line):
                    record_values = []
                    continue
                if record_values is None:
                    continue
                if "}" in line:
                    if len(record_values) >= 2:
                        times.append(record_values[1])
                    record_values = None
                    continue
                match = re.search(r"(\d+)\s*,?\s*$", line)
                if match:
                    record_values.append(int(match.group(1)))
        if times:
            return sorted(times), f"h5dump:{candidate}"
    raise RuntimeError(f"Unable to read eventTime metadata with h5py or h5dump: {last_error}")


def choose_subrange(event_times, requested_count):
    count = min(max(1, requested_count), len(event_times))
    start_index = max(0, (len(event_times) - count) // 2)
    selected = event_times[start_index : start_index + count]
    start_ns = selected[0]
    end_ns = selected[-1] + 1
    return start_ns, end_ns, len(selected), start_index


def parse_reader_count(stdout):
    match = re.search(r"(\d+)\s+events returned\.", stdout)
    if not match:
        return None
    return int(match.group(1))


def run_archive_reader(reader_bin, player_config, archive_dir, chronicle, story, start_ns, end_ns, expect_count):
    command = [
        str(reader_bin),
        "--conf",
        str(player_config),
        "--archive-dir",
        str(archive_dir),
        "--chronicle",
        chronicle,
        "--story",
        story,
        "--start-ns",
        str(start_ns),
        "--end-ns",
        str(end_ns),
        "--expect-count",
        str(expect_count),
        "--exit-after-read",
    ]
    started = time.perf_counter()
    proc = subprocess.run(command, text=True, capture_output=True, timeout=300, check=False)
    elapsed = time.perf_counter() - started
    return command, proc, elapsed


def run_archive_reader_task(task):
    (
        reader_bin,
        player_config,
        archive_dir,
        client_index,
        chronicle,
        story,
        start_ns,
        end_ns,
        expect_count,
    ) = task
    command, proc, elapsed = run_archive_reader(
        reader_bin,
        player_config,
        archive_dir,
        chronicle,
        story,
        start_ns,
        end_ns,
        expect_count,
    )
    return {
        "client_index": client_index,
        "command": command,
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "archive_reader_seconds": elapsed,
        "retrieved_event_count": parse_reader_count(proc.stdout) or 0,
    }


def wait_for_story_archive_event_count(task):
    (
        client_index,
        chronicle,
        story,
        output_dir,
        operation_count,
        archive_wait_seconds,
        poll_interval_seconds,
    ) = task
    archive_count_wait_trace = {}
    story_archive_files, story_archive_count, archive_count_source, archive_count_poll_count = wait_for_archive_event_count(
        Path(output_dir),
        chronicle,
        story,
        operation_count,
        archive_wait_seconds,
        poll_interval_seconds,
        archive_count_wait_trace,
    )
    return {
        "chronicle": chronicle,
        "story": story,
        "client_index": client_index,
        "archive_event_count": story_archive_count,
        "archive_event_count_poll_count": archive_count_poll_count,
        "archive_count_source": archive_count_source,
        "archive_event_count_wait_trace": archive_count_wait_trace,
        "archive_files": [str(path) for path in story_archive_files],
    }


def wait_for_story_archive_publication(task):
    client_index, chronicle, story, output_dir, archive_wait_seconds = task
    started = time.perf_counter()
    started_epoch = time.time()
    files = wait_for_archive_file(Path(output_dir), chronicle, story, archive_wait_seconds)
    return {
        "chronicle": chronicle,
        "story": story,
        "client_index": client_index,
        "archive_publication_files": [str(path) for path in files],
        "archive_publication_file_count": len(files),
        "archive_publication_confirm_seconds": time.perf_counter() - started,
        "archive_publication_confirm_epoch_seconds": time.time(),
        "archive_publication_wait_started_epoch_seconds": started_epoch,
    }


def client_phase_metrics(progresses):
    metrics = {}
    if not progresses:
        return metrics
    phase_names = sorted(
        {
            key.removeprefix("client_phase_").removesuffix("_seconds")
            for progress in progresses
            for key in progress
            if key.startswith("client_phase_")
            and key.endswith("_seconds")
            and not key.endswith("_epoch_seconds")
        }
    )
    for phase in phase_names:
        field = f"client_phase_{phase}_seconds"
        values = [progress.get(field) for progress in progresses if progress.get(field) is not None]
        if not values:
            continue
        metrics[f"{field}_min"] = min(values)
        metrics[f"{field}_max"] = max(values)
        metrics[f"{field}_avg"] = statistics.fmean(values)
    start_values = [
        progress.get("client_phase_started_epoch_seconds")
        for progress in progresses
        if progress.get("client_phase_started_epoch_seconds") is not None
    ]
    if start_values:
        metrics["client_phase_first_started_epoch_seconds"] = min(start_values)
        metrics["client_phase_last_started_epoch_seconds"] = max(start_values)
    return metrics


def write_client_config(base_config_path, output_path, query_port_offset):
    config = json.loads(Path(base_config_path).read_text(encoding="utf-8"))
    query_rpc = config["chrono_client"]["ClientQueryService"]["rpc"]
    query_rpc["service_base_port"] = int(query_rpc["service_base_port"]) + query_port_offset
    Path(output_path).write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Run a ChronoLog archive-backed range retrieval benchmark.")
    parser.add_argument("--client-config", required=True)
    parser.add_argument("--player-config", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--archive-reader-bin", required=True)
    parser.add_argument("--operation-count", type=int, default=1000)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--range-event-count", type=int, default=0)
    parser.add_argument("--node-count", type=int, default=2)
    parser.add_argument("--client-count", type=int, default=1)
    parser.add_argument("--archive-wait-seconds", type=float, default=420.0)
    parser.add_argument("--archive-event-count-poll-interval-seconds", type=float, default=1.0)
    parser.add_argument(
        "--archive-event-count-wait-mode",
        choices=["inline", "parallel_process"],
        default="inline",
        help="How archive event-count validation waits across stories. Default keeps historical inline behavior.",
    )
    parser.add_argument(
        "--archive-readback-mode",
        choices=["inline", "parallel_thread"],
        default="inline",
        help="How independent per-story ChronoPlayer archive readbacks are issued. Default keeps historical inline behavior.",
    )
    args = parser.parse_args()

    result_root = Path(args.result_dir)
    chronolog_dir = result_root / "chronolog"
    output_dir = chronolog_dir / "output"
    chronolog_dir.mkdir(parents=True, exist_ok=True)
    reader_bin = Path(args.archive_reader_bin)
    if not reader_bin.exists() or not os.access(reader_bin, os.X_OK):
        raise RuntimeError(f"Archive reader binary is unavailable or not executable: {reader_bin}")

    suffix = f"{int(time.time() * 1000)}_{os.getpid()}"
    story_specs = [
        (
            client_index,
            f"phase0_archive_range_chronicle_{client_index}_{suffix}",
            f"phase0_archive_range_story_{client_index}_{suffix}",
            result_root if args.client_count == 1 else result_root / "client-progress" / f"client-{client_index}",
        )
        for client_index in range(args.client_count)
    ]

    started = time.perf_counter()
    children = []
    for client_index, chronicle, story, client_result_root in story_specs:
        (client_result_root / "chronolog").mkdir(parents=True, exist_ok=True)
        client_config = client_result_root / "chronolog" / "archive-range-client-conf.json"
        write_client_config(args.client_config, client_config, client_index)
        child = multiprocessing.Process(
            target=run_client_append,
            args=(
                str(client_config),
                str(client_result_root),
                chronicle,
                story,
                args.operation_count,
                args.message_size_bytes,
            ),
        )
        child.start()
        children.append((child, client_index, chronicle, story, client_result_root, client_config))
    for child, _, chronicle, story, _, _ in children:
        child.join()
        if child.exitcode != 0:
            raise RuntimeError(f"archive range append client failed for {chronicle}/{story}: exitcode={child.exitcode}")
    append_clients_finished = time.perf_counter()

    progresses = []
    append_latencies_ms = []
    release_started_values = []
    release_returned_values = []
    for _, client_index, chronicle, story, client_result_root, client_config in children:
        progress_path = client_result_root / "chronolog" / "durable-client-progress.json"
        progress = {}
        if progress_path.exists():
            progress = json.loads(progress_path.read_text(encoding="utf-8"))
        progress["client_index"] = client_index
        progress["chronicle"] = chronicle
        progress["story"] = story
        progress["client_config"] = str(client_config)
        progress["progress_path"] = str(progress_path)
        progresses.append(progress)
        append_latencies_ms.extend(progress.get("append_latencies_ms", []))
        if progress.get("release_started") is not None:
            release_started_values.append(progress["release_started"])
        if progress.get("release_returned") is not None:
            release_returned_values.append(progress["release_returned"])
    release_started_at = min(release_started_values) if release_started_values else None
    release_returned_at = max(release_returned_values) if release_returned_values else None

    archive_started = time.perf_counter()
    archive_started_epoch = time.time()
    archive_publication_started = time.perf_counter()
    archive_publication_started_epoch = time.time()
    publication_tasks = [
        (
            client_index,
            chronicle,
            story,
            str(output_dir),
            args.archive_wait_seconds,
        )
        for _, client_index, chronicle, story, _, _ in children
    ]
    publication_results = [wait_for_story_archive_publication(task) for task in publication_tasks]
    publication_results.sort(key=lambda item: item["client_index"])
    archive_publication_confirm_seconds = time.perf_counter() - archive_publication_started
    archive_publication_confirm_epoch = time.time()
    archive_files = []
    archive_count = 0
    archive_count_sources = []
    archive_count_poll_counts = []
    archive_count_wait_traces = []
    archive_count_validation_started = time.perf_counter()
    archive_count_validation_started_epoch = time.time()
    wait_tasks = [
        (
            client_index,
            chronicle,
            story,
            str(output_dir),
            args.operation_count,
            args.archive_wait_seconds,
            args.archive_event_count_poll_interval_seconds,
        )
        for _, client_index, chronicle, story, _, _ in children
    ]
    if args.archive_event_count_wait_mode == "parallel_process" and len(wait_tasks) > 1:
        with multiprocessing.Pool(processes=len(wait_tasks)) as pool:
            per_story_results = pool.map(wait_for_story_archive_event_count, wait_tasks)
    else:
        per_story_results = [wait_for_story_archive_event_count(task) for task in wait_tasks]
    per_story_results.sort(key=lambda item: item["client_index"])
    for per_story in per_story_results:
        story_archive_files = [Path(path) for path in per_story["archive_files"]]
        archive_files.extend(story_archive_files)
        archive_count += per_story["archive_event_count"]
        archive_count_sources.append(per_story["archive_count_source"])
        archive_count_poll_counts.append(per_story["archive_event_count_poll_count"])
        archive_count_wait_traces.append(per_story["archive_event_count_wait_trace"])
    archive_confirm_seconds = time.perf_counter() - archive_started
    archive_confirm_epoch = time.time()
    archive_count_validation_seconds = time.perf_counter() - archive_count_validation_started
    archive_count_validation_epoch = time.time()

    metadata_selection_errors = []
    timestamp_datasets = []
    reader_commands = []
    reader_returncodes = []
    reader_stdout = []
    reader_stderr = []
    readback_story_seconds_sum = 0.0
    readback_story_seconds_max = 0.0
    replay_seconds = 0.0
    expected_range_count = 0
    retrieved_count = 0
    first_range_start_ns = None
    first_range_end_ns = None
    first_range_start_index = 0
    requested_range_count = args.range_event_count or max(1, min(args.operation_count, args.operation_count // 10))
    readback_tasks = []
    metadata_selection_started = time.perf_counter()
    for per_story in per_story_results:
        story_archive_files = [Path(path) for path in per_story["archive_files"]]
        try:
            event_times, timestamp_dataset = dataset_event_times(story_archive_files)
            start_ns, end_ns, story_expected_range_count, range_start_index = choose_subrange(
                event_times,
                requested_range_count,
            )
            metadata_selection_error = ""
        except Exception as exc:
            metadata_selection_error = str(exc)
            timestamp_dataset = ""
            start_ns = 1
            end_ns = 2_000_000_000_000_000_000
            story_expected_range_count = args.operation_count
            range_start_index = 0
        per_story.update(
            {
                "timestamp_dataset": timestamp_dataset,
                "metadata_subrange_selection_error": metadata_selection_error or None,
                "range_start_ns": start_ns,
                "range_end_ns": end_ns,
                "range_start_index": range_start_index,
                "range_event_count": story_expected_range_count,
            }
        )
        if first_range_start_ns is None:
            first_range_start_ns = start_ns
            first_range_end_ns = end_ns
            first_range_start_index = range_start_index
        if metadata_selection_error:
            metadata_selection_errors.append(metadata_selection_error)
        if timestamp_dataset:
            timestamp_datasets.append(timestamp_dataset)
        expected_range_count += story_expected_range_count
        readback_tasks.append(
            (
                reader_bin,
                Path(args.player_config),
                output_dir,
                per_story["client_index"],
                per_story["chronicle"],
                per_story["story"],
                start_ns,
                end_ns,
                story_expected_range_count,
            )
        )
    metadata_selection_seconds = time.perf_counter() - metadata_selection_started

    readback_started = time.perf_counter()
    if args.archive_readback_mode == "parallel_thread" and len(readback_tasks) > 1:
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(readback_tasks)) as executor:
            readback_results = list(executor.map(run_archive_reader_task, readback_tasks))
    else:
        readback_results = [run_archive_reader_task(task) for task in readback_tasks]
    replay_seconds = time.perf_counter() - readback_started
    readback_results.sort(key=lambda item: item["client_index"])
    readback_result_by_client = {item["client_index"]: item for item in readback_results}
    for per_story in per_story_results:
        readback_result = readback_result_by_client.get(per_story["client_index"])
        if readback_result is None:
            continue
        story_replay_seconds = readback_result["archive_reader_seconds"]
        per_story.update(
            {
                "retrieved_event_count": readback_result["retrieved_event_count"],
                "archive_reader_returncode": readback_result["returncode"],
                "archive_reader_seconds": story_replay_seconds,
            }
        )
        reader_commands.append(readback_result["command"])
        reader_returncodes.append(readback_result["returncode"])
        reader_stdout.append(readback_result["stdout"])
        reader_stderr.append(readback_result["stderr"])
        readback_story_seconds_sum += story_replay_seconds
        readback_story_seconds_max = max(readback_story_seconds_max, story_replay_seconds)
        retrieved_count += readback_result["retrieved_event_count"]

    log_settle_requested_seconds = log_settle_seconds()
    log_settle_started = time.perf_counter()
    time.sleep(log_settle_requested_seconds)
    control_metrics = wait_for_control_path_profiles(chronolog_dir / "logs", args.client_count)
    log_settle_actual_seconds = time.perf_counter() - log_settle_started
    _, first_chronicle, first_story, _ = story_specs[0]
    grapher_stage_times, story_id = grapher_archive_stage_times(chronolog_dir / "logs", first_chronicle, first_story)
    grapher_stage_metrics = grapher_archive_stage_metrics(
        chronolog_dir / "logs",
        [(chronicle, story) for _, chronicle, story, _ in story_specs],
        release_returned_at,
    )
    release_to_stage_seconds = {
        f"release_to_{stage}_seconds": None if release_returned_at is None or ts is None else ts - release_returned_at
        for stage, ts in grapher_stage_times.items()
    }
    total_operation_count = args.operation_count * args.client_count
    workflow_duration_seconds = time.perf_counter() - started
    workflow_active_duration_seconds = max(0.0, workflow_duration_seconds - log_settle_actual_seconds)
    workflow_total_message_throughput = (
        total_operation_count / workflow_duration_seconds if workflow_duration_seconds > 0 else 0.0
    )
    workflow_total_message_active_throughput = (
        total_operation_count / workflow_active_duration_seconds if workflow_active_duration_seconds > 0 else 0.0
    )
    archive_publication_throughput = (
        total_operation_count / archive_publication_confirm_seconds
        if archive_publication_confirm_seconds > 0
        else 0.0
    )
    archive_count_validation_throughput = (
        total_operation_count / archive_count_validation_seconds if archive_count_validation_seconds > 0 else 0.0
    )
    archive_event_count_confirm_throughput = (
        total_operation_count / archive_confirm_seconds if archive_confirm_seconds > 0 else 0.0
    )
    range_readback_throughput = expected_range_count / replay_seconds if replay_seconds > 0 else 0.0
    range_readback_latency_ms = replay_seconds * 1000
    success = (
        all(returncode == 0 for returncode in reader_returncodes)
        and archive_count >= total_operation_count
        and retrieved_count == expected_range_count
    )
    metrics = {
        "system": "chronolog",
        "workflow": "archive_range_retrieval",
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
        "duration_seconds": workflow_duration_seconds,
        "throughput_ops_per_sec": range_readback_throughput,
        "throughput_semantics": "range_readback_throughput_ops_per_sec",
        "workflow_duration_seconds": workflow_duration_seconds,
        "workflow_total_message_throughput_ops_per_sec": workflow_total_message_throughput,
        "workflow_active_duration_seconds": workflow_active_duration_seconds,
        "workflow_total_message_active_throughput_ops_per_sec": workflow_total_message_active_throughput,
        "archive_range_append_clients_seconds": append_clients_finished - started,
        "archive_range_metadata_selection_seconds": metadata_selection_seconds,
        "archive_log_settle_seconds": log_settle_requested_seconds,
        "archive_log_settle_actual_seconds": log_settle_actual_seconds,
        "archive_publication_throughput_ops_per_sec": archive_publication_throughput,
        "archive_count_validation_throughput_ops_per_sec": archive_count_validation_throughput,
        "archive_event_count_confirm_throughput_ops_per_sec": archive_event_count_confirm_throughput,
        "range_readback_duration_seconds": replay_seconds,
        "archive_readback_mode": args.archive_readback_mode,
        "range_readback_story_seconds_sum": readback_story_seconds_sum,
        "range_readback_story_seconds_max": readback_story_seconds_max,
        "range_readback_throughput_ops_per_sec": range_readback_throughput,
        "range_readback_event_count": expected_range_count,
        "range_readback_latency_ms": range_readback_latency_ms,
        "latency_semantics": "range_readback_total_duration_ms",
        "avg_latency_ms": range_readback_latency_ms,
        "p50_latency_ms": range_readback_latency_ms,
        "p95_latency_ms": range_readback_latency_ms,
        "p99_latency_ms": range_readback_latency_ms,
        "success": success,
        "semantic_boundary": "archive_storage_range_retrieval",
        "append_ack_boundary": "StoryHandle.log_event_return_then_release_before_archive_wait",
        "durability_boundary": "archive_file_event_count_then_chronoplayer_hdf5_readback",
        "read_path": "chronoplayer_hdf5_archive_reader",
        "storage_backend": "chronolog_grapher_hdf5_archive",
        "chronolog_completion_mode": "archive_readback",
        "archive_wait_started_epoch_seconds": archive_started_epoch,
        "archive_publication_wait_started_epoch_seconds": archive_publication_started_epoch,
        "archive_publication_confirm_seconds": archive_publication_confirm_seconds,
        "archive_publication_confirm_epoch_seconds": archive_publication_confirm_epoch,
        "archive_publication_story_count": len(publication_results),
        "archive_publication_file_count": sum(
            item["archive_publication_file_count"] for item in publication_results
        ),
        "archive_publication_results": publication_results,
        "archive_count_validation_started_epoch_seconds": archive_count_validation_started_epoch,
        "archive_count_validation_seconds": archive_count_validation_seconds,
        "archive_count_validation_epoch_seconds": archive_count_validation_epoch,
        "archive_publication_to_count_validation_seconds": archive_count_validation_seconds,
        "archive_event_count_confirm_seconds": archive_confirm_seconds,
        "archive_event_count_confirm_epoch_seconds": archive_confirm_epoch,
        "archive_event_count_poll_interval_seconds": args.archive_event_count_poll_interval_seconds,
        "archive_event_count_poll_count": sum(archive_count_poll_counts),
        "archive_event_count_poll_counts": archive_count_poll_counts,
        "archive_event_count_wait_mode": args.archive_event_count_wait_mode,
        "archive_event_count_wait_traces": archive_count_wait_traces,
        **archive_event_count_wait_trace_metrics(archive_count_wait_traces),
        "archive_event_count": archive_count,
        "archive_count_source": ",".join(sorted(set(archive_count_sources))),
        "archive_files": [str(path) for path in archive_files],
        **archive_file_stat_metrics(archive_files, release_returned_at, archive_confirm_epoch),
        "timestamp_dataset": ",".join(sorted(set(timestamp_datasets))),
        "metadata_subrange_selection_error": "; ".join(metadata_selection_errors) or None,
        "range_start_ns": first_range_start_ns,
        "range_end_ns": first_range_end_ns,
        "range_start_index": first_range_start_index,
        "range_event_count": expected_range_count,
        "retrieved_event_count": retrieved_count,
        "expected_retrieved_event_count": expected_range_count,
        "readback_event_count": retrieved_count,
        "readback_path": "chronoplayer_hdf5_archive_reader",
        "archive_reader_command": reader_commands,
        "archive_reader_returncode": max(reader_returncodes) if reader_returncodes else 1,
        "archive_reader_returncodes": reader_returncodes,
        "archive_reader_stdout": "\n".join(reader_stdout),
        "archive_reader_stderr": "\n".join(reader_stderr),
        "per_story_results": per_story_results,
        "client_progress": progresses,
        **client_phase_metrics(progresses),
        "append_avg_latency_ms": statistics.fmean(append_latencies_ms) if append_latencies_ms else None,
        "append_p50_latency_ms": percentile(append_latencies_ms, 50),
        "append_p95_latency_ms": percentile(append_latencies_ms, 95),
        "append_p99_latency_ms": percentile(append_latencies_ms, 99),
        "release_seconds": None
        if release_started_at is None or release_returned_at is None
        else release_returned_at - release_started_at,
        "release_returned_epoch_seconds": release_returned_at,
        "story_id": story_id,
        "grapher_archive_stage_epoch_seconds": grapher_stage_times,
        **grapher_stage_metrics,
        **grapher_hdf5_write_profile_metrics(chronolog_dir / "logs"),
        **control_metrics,
        **keeper_to_grapher_drain_metrics(chronolog_dir / "logs", story_id, release_returned_at),
        **grapher_orphan_chunk_metrics(chronolog_dir / "logs", story_id, release_returned_at),
        **release_to_stage_seconds,
        "readback_note": "Range retrieval is executed by ChronoPlayer HDF5ArchiveReadingAgent against archived HDF5 files after event-count evidence. It does not use Keeper live-tail replay.",
    }
    (chronolog_dir / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    if not success:
        raise SystemExit(1)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"chronolog archive range retrieval benchmark failed: {exc}", file=sys.stderr)
        raise
