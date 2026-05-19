#!/usr/bin/env python3
"""Generate graph-ready ChronoLog optimization evolution artifacts."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]


ITERATION_FIELDS = [
    "timestamp",
    "iteration",
    "system",
    "workflow",
    "node_count",
    "operation_count",
    "message_size_bytes",
    "result_dir",
    "throughput_ops_per_sec",
    "p99_latency_ms",
    "correctness",
    "change_summary",
    "profile_evidence",
    "decision",
]

EVOLUTION_FIELDS = [
    "timestamp",
    "iteration",
    "system",
    "workflow",
    "node_count",
    "operation_count",
    "message_size_bytes",
    "total_payload_bytes",
    "throughput_ops_per_sec",
    "throughput_raw",
    "p99_latency_ms",
    "correctness",
    "decision",
    "decision_class",
    "is_smoke_scale",
    "is_accepted_evidence",
    "result_dir",
    "profile_evidence",
    "change_summary",
]

SEMANTIC_FIELDS = [
    "workload",
    "cell",
    "size",
    "status",
    "node_count",
    "nodes",
    "client_count",
    "parallel_clients",
    "parallel_client_count",
    "message_size_bytes",
    "operation_count",
    "operation_count_per_client",
    "message_count_per_client",
    "messages_per_client",
    "total_operation_count",
    "throughput_ops_per_sec",
    "measurement_throughput_field",
    "append_wall_throughput_ops_per_sec",
    "tail_read_count",
    "tail_success_count",
    "tail_max_retrieved_count",
    "tail_final_retrieved_count",
    "tail_avg_latency_ms",
    "tail_p50_latency_ms",
    "tail_p95_latency_ms",
    "tail_p99_latency_ms",
    "tail_read_mode",
    "tail_reader_start_mode",
    "tail_retrieval_throughput_ops_per_sec",
    "tail_rpc_active_throughput_ops_per_sec",
    "packed_tail_payload_bytes",
    "client_tail_rpc_count",
    "client_tail_rpc_success_count",
    "client_tail_rpc_event_count",
    "client_tail_rpc_payload_bytes",
    "client_tail_rpc_avg_us",
    "client_tail_rpc_max_us",
    "client_tail_rpc_payload_mb_per_sec",
    "client_tail_rpc_buffer_alloc_us_avg",
    "client_tail_rpc_bulk_expose_us_avg",
    "client_tail_rpc_payload_move_us_avg",
    "client_tail_rpc_total_us_avg",
    "client_replay_request_batch_count_avg",
    "client_replay_request_batch_count_max",
    "client_replay_request_batch_count_p95",
    "keeper_tail_bulk_count",
    "keeper_tail_bulk_success_count",
    "keeper_tail_bulk_event_count",
    "keeper_tail_bulk_payload_bytes",
    "keeper_tail_bulk_collect_us_avg",
    "keeper_tail_bulk_bulk_expose_us_avg",
    "keeper_tail_bulk_bulk_transfer_us_avg",
    "keeper_tail_bulk_response_us_avg",
    "keeper_tail_bulk_total_us_avg",
    "keeper_tail_bulk_payload_mb_per_sec",
    "keeper_tail_bulk_per_keeper_count",
    "keeper_tail_bulk_per_keeper_payload_mb_per_sec_min",
    "keeper_tail_bulk_per_keeper_payload_mb_per_sec_max",
    "keeper_tail_bulk_per_keeper_payload_mb_per_sec_ratio",
    "keeper_tail_bulk_slowest_transfer_log",
    "keeper_tail_bulk_slowest_transfer_us_total",
    "keeper_tail_bulk_slowest_total_log",
    "keeper_tail_bulk_slowest_total_us_total",
    "keeper_tail_bulk_batch_count_avg",
    "keeper_tail_bulk_batch_count_max",
    "keeper_tail_bulk_batch_count_p95",
    "keeper_tail_bulk_stream_count",
    "keeper_tail_bulk_stream_batch_count_total",
    "keeper_journal_tail_read_count",
    "keeper_journal_tail_matched_events",
    "keeper_journal_tail_snapshot_avg_us",
    "keeper_journal_tail_snapshot_lock_wait_avg_us",
    "keeper_journal_tail_payload_read_avg_us",
    "keeper_journal_tail_payload_read_max_us",
    "keeper_journal_tail_sort_avg_us",
    "keeper_journal_tail_sort_max_us",
    "keeper_journal_tail_payload_alloc_avg_us",
    "keeper_journal_tail_payload_alloc_max_us",
    "keeper_journal_tail_payload_syscall_avg_us",
    "keeper_journal_tail_payload_syscall_max_us",
    "keeper_journal_tail_payload_memcpy_avg_us",
    "keeper_journal_tail_payload_memcpy_max_us",
    "keeper_journal_tail_event_build_avg_us",
    "keeper_journal_tail_event_build_max_us",
    "keeper_journal_tail_read_group_count",
    "keeper_journal_tail_read_group_avg",
    "keeper_journal_tail_read_group_max",
    "keeper_journal_tail_physical_read_bytes",
    "keeper_journal_tail_payload_copy_bytes",
    "keeper_journal_tail_overread_bytes",
    "keeper_journal_tail_direct_read_count",
    "keeper_journal_tail_coalesced_read_count",
    "keeper_journal_tail_vectored_read_count",
    "total_message_count",
    "total_messages",
    "total_payload_bytes",
    "semantic_boundary",
    "append_ack_boundary",
    "durability_boundary",
    "producer_wait_mode",
    "producer_flush_mode",
    "storage_backend",
    "read_path",
    "chronolog_completion_mode",
    "throughput_semantics",
    "latency_semantics",
    "semantic_notes",
    "profile_mode",
    "chronolog_producer_outstanding",
    "chronolog_producer_batch_size",
    "chronolog_producer_wait_policy",
    "chronolog_producer_wait_mode",
    "chronolog_bench_bound_keeper_futures",
    "chronolog_bench_owned_batch",
    "chronolog_bench_owned_batch_min_message_size",
    "chronolog_client_batch_keeper_selection",
    "chronolog_client_parallel_tail_rpc",
    "chronolog_client_keeper_cursor_drain",
    "chronolog_client_keeper_cursor_drain_max_batches",
    "chronolog_client_keeper_cursor_packed_batch",
    "chronolog_client_keeper_cursor_metadata_only_output",
    "chronolog_client_keeper_cursor_packed_bulk",
    "chronolog_client_keeper_cursor_packed_bulk_stream",
    "chronolog_client_keeper_cursor_packed_bulk_stream_max_batches",
    "chronolog_client_keeper_cursor_packed_bulk_buffer_bytes",
    "chronolog_keeper_tail_packed_bulk_uninitialized_buffer",
    "chronolog_keeper_tail_packed_bulk_chunk_bytes",
    "chronolog_keeper_tail_packed_bulk_parallel_transfer",
    "chronolog_mixed_tail_read_mode",
    "chronolog_client_execution_mode",
    "chronolog_mixed_tail_writer_release_story",
    "chronolog_archive_event_count_poll_interval_seconds",
    "chronolog_archive_event_count_wait_mode",
    "chronolog_hdf5_archive_atomic_rename",
    "chronolog_hdf5_archive_chunk_events",
    "chronolog_hdf5_archive_layout",
    "chronolog_raw_blob_preallocate",
    "chronolog_raw_blob_async_close",
    "chronolog_raw_blob_async_publish",
    "chronolog_raw_blob_async_publish_threads",
    "chronolog_raw_blob_publish_before_close",
    "chronolog_raw_blob_sidecar_meta",
    "workflow_active_duration_seconds",
    "workflow_total_message_active_throughput_ops_per_sec",
    "archive_range_append_clients_seconds",
    "archive_range_metadata_selection_seconds",
    "archive_publication_confirm_seconds",
    "archive_publication_throughput_ops_per_sec",
    "archive_publication_story_count",
    "archive_publication_file_count",
    "archive_count_validation_seconds",
    "archive_count_validation_throughput_ops_per_sec",
    "archive_publication_to_count_validation_seconds",
    "archive_event_count_confirm_seconds",
    "archive_event_count_confirm_throughput_ops_per_sec",
    "archive_event_count",
    "readback_event_count",
    "archive_readback_mode",
    "timestamp_dataset",
    "client_phase_config_loaded_seconds_max",
    "client_phase_connect_returned_seconds_max",
    "client_phase_create_chronicle_returned_seconds_max",
    "client_phase_acquire_story_returned_seconds_max",
    "client_phase_append_loop_finished_seconds_max",
    "client_phase_release_story_returned_seconds_max",
    "range_readback_throughput_ops_per_sec",
    "range_readback_duration_seconds",
    "range_readback_event_count",
    "range_readback_story_seconds_sum",
    "range_readback_story_seconds_max",
    "archive_event_count_wait_story_count",
    "archive_event_count_wait_mode",
    "archive_event_count_wait_poll_count_total",
    "archive_event_count_wait_confirm_seconds_max",
    "archive_event_count_wait_first_file_seen_seconds_max",
    "archive_event_count_wait_first_manifest_count_seconds_max",
    "archive_event_count_wait_first_hdf5_count_seconds_max",
    "archive_event_count_wait_first_complete_count_seconds_max",
    "archive_event_count_wait_manifest_count_seconds_total_max",
    "archive_event_count_wait_manifest_count_seconds_max_max",
    "archive_event_count_wait_hdf5_count_seconds_total_max",
    "archive_event_count_wait_hdf5_count_seconds_max_max",
    "archive_event_count_wait_grapher_log_count_seconds_total_max",
    "archive_event_count_wait_grapher_log_count_seconds_max_max",
    "chronolog_grapher_stop_story_archive_drain",
    "chronolog_grapher_stop_story_archive_drain_timeout_ms",
    "keeper_stop_story_flush_drain_async_complete",
    "keeper_journal_mode",
    "keeper_journal_placement",
    "keeper_journal_local_base",
    "keeper_journal_shards",
    "keeper_journal_shard_policy",
    "keeper_journal_batch_writev",
    "keeper_journal_move_batch_payloads",
    "keeper_journal_tail_read_mode",
    "keeper_journal_tail_direct_read_into_events",
    "keeper_tail_batch_max_events",
    "keeper_tail_batch_max_bytes",
    "keeper_journal_group_commit_flush_events",
    "keeper_journal_group_commit_strict_flush_event_cap",
    "keeper_journal_group_commit_wait_us",
    "keeper_journal_group_commit_flush_wait_us",
    "keeper_journal_group_commit_queue_boundary_min_events",
    "keeper_journal_group_commit_queue_boundary_min_payload_bytes",
    "keeper_journal_group_commit_queue_boundary_wait_every_n",
    "keeper_journal_notify_owner_only_on_empty",
    "keeper_journal_async_drain_threads",
    "keeper_journal_async_batch_completion_dispatch",
    "keeper_journal_callback_dispatch_threads",
    "keeper_journal_callback_batch_drain",
    "keeper_journal_callback_batch_drain_max",
    "keeper_journal_callback_batch_drain_min_payload_bytes",
    "keeper_journal_durable_complete_before_publish",
    "keeper_wal_drain_batch_events",
    "keeper_wal_drain_batch_wait_us",
    "keeper_recording_margo_progress_thread",
    "visor_parallel_keeper_stop",
    "visor_release_story_profile_count",
    "visor_release_story_total_us_max",
    "visor_release_story_notify_recording_stop_us_max",
    "registry_recording_stop_profile_count",
    "registry_recording_stop_keeper_notify_us_max",
    "registry_recording_stop_grapher_notify_us_max",
    "registry_keeper_stop_rpc_profile_count",
    "registry_keeper_stop_rpc_parallel_max",
    "registry_keeper_stop_rpc_target_count_max",
    "registry_keeper_stop_rpc_total_us_max",
    "registry_keeper_stop_rpc_rpc_max_us_max",
    "registry_keeper_stop_rpc_rpc_sum_us_max",
    "registry_grapher_stop_rpc_profile_count",
    "registry_grapher_stop_rpc_expected_keeper_drains_max",
    "registry_grapher_stop_rpc_rpc_us_max",
    "keeper_stop_story_profile_count",
    "keeper_stop_story_total_us_max",
    "keeper_stop_story_flush_story_us_max",
    "keeper_drain_profile_count",
    "keeper_drain_profile_rpc_max_us",
    "keeper_drain_profile_total_max_us",
    "grapher_receive_profile_count",
    "grapher_receive_profile_bulk_transfer_max_us",
    "grapher_receive_profile_deserialization_max_us",
    "grapher_receive_profile_ingestion_max_us",
    "grapher_receive_profile_total_with_response_max_us",
    "grapher_stop_story_profile_count",
    "grapher_stop_story_expected_keeper_drains_max",
    "grapher_stop_story_total_us_max",
    "grapher_archive_drain_count",
    "grapher_archive_drain_wait_max_us",
    "grapher_archive_drain_wait_total_us",
    "grapher_archive_drain_queued_nonzero_count",
    "grapher_archive_drain_inflight_nonzero_count",
    "grapher_story_drain_complete_count",
    "grapher_story_drain_complete_max_count",
    "grapher_stop_drain_complete_wait_count",
    "grapher_stop_drain_complete_wait_success_count",
    "grapher_stop_drain_complete_wait_async_count",
    "grapher_stop_drain_complete_wait_max_us",
    "grapher_stop_drain_wait_outside_lock",
    "chronolog_grapher_stop_drain_wait_outside_lock",
    "grapher_async_stop_retire_count",
    "grapher_async_stop_retire_success_count",
    "grapher_async_stop_retire_wait_max_us",
    "grapher_stop_retire_profile_count",
    "grapher_stop_retire_profile_async_count",
    "grapher_stop_retire_initial_lock_wait_max_us",
    "grapher_stop_retire_initial_lock_hold_max_us",
    "grapher_stop_retire_completion_wait_max_us",
    "grapher_stop_retire_collect_erase_max_us",
    "grapher_stop_retire_async_lock_wait_max_us",
    "grapher_stop_retire_finalize_max_us",
    "grapher_stop_retire_archive_drain_wait_max_us",
    "grapher_stop_retire_total_max_us",
    "grapher_archive_stage_story_count",
    "grapher_hdf5_processing_count",
    "grapher_hdf5_written_count",
    "release_to_grapher_hdf5_processing_first_seconds",
    "release_to_grapher_hdf5_processing_last_seconds",
    "release_to_grapher_hdf5_written_first_seconds",
    "release_to_grapher_hdf5_written_last_seconds",
    "grapher_hdf5_write_profile_count",
    "grapher_hdf5_write_profile_event_count",
    "grapher_hdf5_write_profile_raw_payload_bytes_total",
    "grapher_hdf5_writer_total_us_avg",
    "grapher_hdf5_writer_total_us_max",
    "grapher_hdf5_write_us_avg",
    "grapher_hdf5_write_us_max",
    "grapher_hdf5_dataset_write_call_us_avg",
    "grapher_hdf5_dataset_write_call_us_max",
    "grapher_hdf5_dataset_payload_write_call_us_avg",
    "grapher_hdf5_dataset_payload_write_call_us_max",
    "grapher_hdf5_raw_payload_writev_us_avg",
    "grapher_hdf5_raw_payload_writev_us_max",
    "grapher_hdf5_raw_payload_close_us_avg",
    "grapher_hdf5_raw_payload_close_us_max",
    "grapher_hdf5_raw_payload_close_wait_us_avg",
    "grapher_hdf5_raw_payload_close_wait_us_max",
    "grapher_hdf5_raw_payload_async_close_avg",
    "grapher_hdf5_raw_payload_async_close_max",
    "grapher_hdf5_open_us_max",
    "grapher_hdf5_rename_us_max",
    "grapher_hdf5_publish_rename_us_max",
    "grapher_hdf5_archive_manifest_write_us_max",
    "grapher_async_archive_publish_count",
    "grapher_async_archive_publish_success_count",
    "grapher_async_archive_publish_close_wait_us",
    "grapher_async_archive_publish_close_us",
    "grapher_async_archive_publish_rename_us",
    "grapher_async_archive_publish_manifest_us",
    "grapher_async_archive_publish_total_us",
    "client_append_submit_call_count",
    "client_append_submit_event_count",
    "client_append_submit_avg_us",
    "client_append_submit_max_us",
    "client_append_future_count",
    "client_append_future_count_avg_per_submit",
    "client_append_future_count_max_per_submit",
    "client_append_future_wait_count",
    "client_append_future_wait_avg_us",
    "client_append_future_wait_max_us",
    "client_replay_stats_count",
    "client_replay_event_count",
    "client_replay_payload_bytes",
    "client_replay_rpc_collect_us_avg",
    "client_replay_rpc_collect_us_max",
    "client_replay_sort_us_avg",
    "client_replay_sort_us_max",
    "client_replay_output_build_us_avg",
    "client_replay_output_build_us_max",
    "client_replay_request_batch_count_avg",
    "client_replay_request_batch_count_max",
    "client_replay_request_batch_count_p95",
    "mofka_partition_type",
    "mofka_storage_target_type",
    "mofka_producer_wait_mode",
    "mofka_producer_flush_mode",
    "kafka_acks",
    "metrics_path",
    "notes",
]


def rel(path: Path) -> str:
    return str(path.relative_to(REPO_ROOT))


def parse_int(value: str) -> int | None:
    value = (value or "").strip()
    if not value:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def parse_float(value: str) -> float | None:
    value = (value or "").strip()
    if not value:
        return None
    try:
        return float(value)
    except ValueError:
        return None


def decision_class(decision: str) -> str:
    text = (decision or "").lower()
    if text.startswith(("accepted", "baseline", "guarded", "metric-row")):
        return "accepted"
    if text.startswith(("rejected", "reverted", "superseded")):
        return "rejected"
    if "default-off" in text or "do-not-promote" in text:
        return "rejected"
    if text.startswith(("mixed", "partial")) or "caveat" in text:
        return "caveated"
    if text in {"n/a", "na"}:
        return "design"
    return "other"


def row_is_accepted(row: dict[str, str]) -> bool:
    if (row.get("correctness") or "").lower() != "pass":
        return False
    klass = decision_class(row.get("decision", ""))
    return klass in {"accepted", "caveated"}


def iteration_order(row: dict[str, Any]) -> tuple[int, str]:
    iteration = str(row.get("iteration", ""))
    matches = re.findall(r"(\d+)", iteration)
    if matches:
        return (int(matches[-1]), iteration)
    return (-1, iteration)


def is_smoke_scale(row: dict[str, str]) -> bool:
    operation_count = row.get("operation_count") or ""
    counts: list[int] = []
    for part in operation_count.replace(";", " ").split():
        parsed = parse_int(part)
        if parsed is not None:
            counts.append(parsed)
    return bool(counts) and max(counts) <= 1000


def total_payload_bytes(row: dict[str, str]) -> int | None:
    operation_count = parse_int(row.get("operation_count") or "")
    message_size = parse_int(row.get("message_size_bytes") or "")
    if operation_count is None or message_size is None:
        return None
    return operation_count * message_size


def load_iteration_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        missing = [field for field in ITERATION_FIELDS if field not in (reader.fieldnames or [])]
        if missing:
            raise ValueError(f"{rel(path)} missing expected columns: {', '.join(missing)}")
        return [dict(row) for row in reader]


def normalize_iteration_row(row: dict[str, str]) -> dict[str, Any]:
    normalized = {field: row.get(field, "") for field in EVOLUTION_FIELDS}
    normalized["total_payload_bytes"] = total_payload_bytes(row)
    normalized["throughput_raw"] = row.get("throughput_ops_per_sec", "")
    throughput = parse_float(row.get("throughput_ops_per_sec", ""))
    p99 = parse_float(row.get("p99_latency_ms", ""))
    # Some historical rows used the throughput column for a prose summary and
    # put the numeric throughput in the next field. Keep graph output numeric.
    if throughput is None and p99 is not None and (row.get("correctness") or "").lower() == "pass":
        throughput = p99
        p99 = None
    normalized["throughput_ops_per_sec"] = "" if throughput is None else throughput
    normalized["p99_latency_ms"] = "" if p99 is None else p99
    normalized["decision_class"] = decision_class(row.get("decision", ""))
    normalized["is_smoke_scale"] = is_smoke_scale(row)
    normalized["is_accepted_evidence"] = row_is_accepted(row)
    return normalized


def write_csv(path: Path, rows: list[dict[str, Any]], fields: list[str]) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def flatten_manifest(manifest_path: Path) -> list[dict[str, Any]]:
    manifest = json.loads(manifest_path.read_text())
    rows: list[dict[str, Any]] = []
    for workload, workload_data in manifest.get("workloads", {}).items():
        for cell in workload_data.get("cells", []):
            if cell.get("status") != "accepted":
                rows.append(
                    {
                        "workload": workload,
                        "cell": cell.get("cell"),
                        "size": "all",
                        "status": cell.get("status"),
                        "notes": cell.get("reason") or cell.get("notes") or "",
                    }
                )
                continue
            for size, metrics in cell.get("rows", {}).items():
                row = {field: "" for field in SEMANTIC_FIELDS}
                row.update({key: metrics.get(key, "") for key in SEMANTIC_FIELDS if key in metrics})
                row.update(
                    {
                        "workload": workload,
                        "cell": cell.get("cell"),
                        "size": size,
                        "status": cell.get("status"),
                        "notes": cell.get("notes", ""),
                    }
                )
                rows.append(row)
    return rows


def write_summary(
    output_dir: Path,
    all_rows: list[dict[str, Any]],
    chronolog_rows: list[dict[str, Any]],
    semantic_rows: list[dict[str, Any]],
) -> None:
    latest_decision_by_workload_size: dict[tuple[str, str], dict[str, Any]] = {}
    for row in all_rows:
        if row.get("system") != "chronolog":
            continue
        if str(row.get("correctness", "")).lower() != "pass":
            continue
        key = (str(row.get("workflow", "")), str(row.get("message_size_bytes", "")))
        prior = latest_decision_by_workload_size.get(key)
        if prior is None or iteration_order(row) > iteration_order(prior):
            latest_decision_by_workload_size[key] = row

    latest_by_workload_size: dict[tuple[str, str], dict[str, Any]] = {}
    for row in chronolog_rows:
        if row.get("throughput_ops_per_sec") in {"", None}:
            continue
        key = (str(row.get("workflow", "")), str(row.get("message_size_bytes", "")))
        prior = latest_by_workload_size.get(key)
        if prior is None or iteration_order(row) > iteration_order(prior):
            latest_by_workload_size[key] = row

    accepted_counts = defaultdict(int)
    other_accepted_count = 0
    for row in all_rows:
        if not row.get("is_accepted_evidence"):
            continue
        system = str(row.get("system", ""))
        if system in {"chronolog", "kafka", "mofka"}:
            accepted_counts[system] += 1
        else:
            other_accepted_count += 1

    lines = [
        "# ChronoLog Optimization Evolution Report",
        "",
        f"Timestamp: {datetime.now().astimezone().strftime('%Y-%m-%d %H:%M %Z')}",
        "",
        "Status: complete",
        "",
        "Artifacts:",
        "",
        "- `iteration-evolution.csv`: normalized iteration log for plotting all systems and decisions.",
        "- `chronolog-accepted-evolution.csv`: accepted or caveated ChronoLog evidence only.",
        "- `latest-semantic-matrix.csv`: flattened current six-way manifest when a manifest is provided.",
        "- The latest-decision table below includes non-promoted/rejected ChronoLog rows so default decisions do not get hidden by earlier accepted candidates.",
        "",
        "Accepted Evidence Counts:",
        "",
        "| System | Rows |",
        "|---|---:|",
    ]
    for system in sorted(accepted_counts):
        lines.append(f"| {system or 'unknown'} | {accepted_counts[system]} |")
    lines.append(f"| other/mixed/control-plane | {other_accepted_count} |")

    lines.extend(
        [
            "",
            "Latest ChronoLog Decisions By Workflow And Size:",
            "",
            "| Workflow | Size | Iteration | Throughput | Decision Class | Decision | Result |",
            "|---|---:|---|---:|---|---|---|",
        ]
    )
    for (workflow, size), row in sorted(latest_decision_by_workload_size.items()):
        throughput = row.get("throughput_ops_per_sec") or ""
        lines.append(
            f"| {workflow} | {size or ''} | {row.get('iteration', '')} | {throughput} | "
            f"{row.get('decision_class', '')} | {row.get('decision', '')} | `{row.get('result_dir', '')}` |"
        )

    lines.extend(
        [
            "",
            "Latest Accepted ChronoLog Rows By Workflow And Size:",
            "",
            "| Workflow | Size | Iteration | Throughput | Decision | Result |",
            "|---|---:|---|---:|---|---|",
        ]
    )
    for (workflow, size), row in sorted(latest_by_workload_size.items()):
        throughput = row.get("throughput_ops_per_sec") or ""
        lines.append(
            f"| {workflow} | {size or ''} | {row.get('iteration', '')} | {throughput} | "
            f"{row.get('decision', '')} | `{row.get('result_dir', '')}` |"
        )

    lines.extend(
        [
            "",
            "Semantic Matrix Coverage:",
            "",
            f"- flattened_rows: {len(semantic_rows)}",
            "- Use the `cell`, `semantic_boundary`, `append_ack_boundary`, and `durability_boundary` columns before comparing throughput.",
            "- Smoke-scale rows remain marked in `iteration-evolution.csv` and must not be used as final claims.",
        ]
    )
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iteration-log", default=".agent/results/profileforge-iteration-log.csv")
    parser.add_argument("--manifest-json", default="")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    output_dir = REPO_ROOT / args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    iteration_log = REPO_ROOT / args.iteration_log
    rows = [normalize_iteration_row(row) for row in load_iteration_rows(iteration_log)]
    chronolog_rows = [
        row for row in rows if row["system"] == "chronolog" and row["is_accepted_evidence"]
    ]

    semantic_rows: list[dict[str, Any]] = []
    if args.manifest_json:
        semantic_rows = flatten_manifest(REPO_ROOT / args.manifest_json)

    write_csv(output_dir / "iteration-evolution.csv", rows, EVOLUTION_FIELDS)
    write_csv(output_dir / "chronolog-accepted-evolution.csv", chronolog_rows, EVOLUTION_FIELDS)
    if semantic_rows:
        write_csv(output_dir / "latest-semantic-matrix.csv", semantic_rows, SEMANTIC_FIELDS)
    write_summary(output_dir, rows, chronolog_rows, semantic_rows)

    print(rel(output_dir / "summary.md"))
    print(rel(output_dir / "iteration-evolution.csv"))
    print(rel(output_dir / "chronolog-accepted-evolution.csv"))
    if semantic_rows:
        print(rel(output_dir / "latest-semantic-matrix.csv"))


if __name__ == "__main__":
    main()
