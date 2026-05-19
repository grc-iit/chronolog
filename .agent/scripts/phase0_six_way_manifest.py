#!/usr/bin/env python3
"""Write the Phase 0 six-way benchmark semantics manifest."""

from __future__ import annotations

import argparse
import json
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
REQUIRED_COUNTS = [
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
    "total_message_count",
    "total_messages",
    "total_payload_bytes",
]

SEMANTIC_KNOBS = [
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
    "tail_read_mode",
    "tail_reader_start_mode",
    "chronolog_client_execution_mode",
    "chronolog_mixed_tail_writer_release_story",
    "chronolog_data_collection_poll_interval_us",
    "chronolog_archive_event_count_poll_interval_seconds",
    "chronolog_keeper_data_collection_streams",
    "chronolog_keeper_data_collection_threads_per_stream",
    "chronolog_grapher_data_collection_streams",
    "chronolog_grapher_data_collection_threads_per_stream",
    "chronolog_grapher_inactive_story_delay_seconds",
    "chronolog_grapher_retire_on_stop",
    "chronolog_grapher_stop_story_archive_drain",
    "chronolog_grapher_stop_story_archive_drain_timeout_ms",
    "chronolog_grapher_extraction_threads",
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
    "keeper_journal_mode",
    "keeper_journal_placement",
    "keeper_journal_local_base",
    "keeper_journal_shards",
    "keeper_journal_shard_policy",
    "keeper_journal_fdatasync_batch_events",
    "keeper_journal_batch_writev",
    "keeper_journal_move_batch_payloads",
    "keeper_journal_tail_read_mode",
    "keeper_journal_tail_direct_read_into_events",
    "keeper_tail_batch_max_events",
    "keeper_tail_batch_max_bytes",
    "keeper_journal_single_writer",
    "keeper_journal_single_writer_batch_events",
    "keeper_journal_group_commit_wait",
    "keeper_journal_group_commit_flush_events",
    "keeper_journal_group_commit_flush_bytes",
    "keeper_journal_group_commit_strict_flush_event_cap",
    "keeper_journal_group_commit_large_payload_bytes",
    "keeper_journal_group_commit_large_payload_flush_events",
    "keeper_journal_group_commit_wait_us",
    "keeper_journal_group_commit_flush_wait_us",
    "keeper_journal_group_commit_queue_boundary_min_events",
    "keeper_journal_group_commit_queue_boundary_min_payload_bytes",
    "keeper_journal_group_commit_queue_boundary_wait_every_n",
    "keeper_journal_notify_owner_only_on_empty",
    "chronolog_client_keeper_time_bucket_ns",
    "keeper_journal_async_callback_dispatch",
    "keeper_journal_async_batch_completion_dispatch",
    "keeper_journal_callback_dispatch_threads",
    "keeper_journal_callback_batch_drain",
    "keeper_journal_callback_batch_drain_max",
    "keeper_journal_callback_batch_drain_min_payload_bytes",
    "keeper_journal_async_drain_threads",
    "keeper_wal_drain_batch_events",
    "keeper_wal_drain_batch_wait_us",
    "keeper_journal_durable_complete_before_publish",
    "keeper_append_stats_interval_events",
    "keeper_recording_margo_xstreams",
    "keeper_recording_margo_progress_thread",
    "keeper_recording_margo_handlers",
    "grapher_recording_margo_xstreams",
    "grapher_recording_margo_handlers",
    "grapher_direct_deserialize",
    "keeper_drain_margo_progress_thread",
    "keeper_drain_margo_rpc_threads",
    "keeper_extraction_threads",
    "keeper_direct_serialize",
    "keeper_fast_wire",
    "keeper_stop_story_flush_drain",
    "keeper_stop_story_flush_drain_timeout_ms",
    "keeper_stop_story_flush_drain_async_complete",
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
    "chronolog_archive_event_count_wait_mode",
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
    "deploy_stop_timeout_seconds",
    "mofka_partition_type",
    "mofka_storage_target_type",
    "mofka_storage_target_size",
    "mofka_producer_wait_mode",
    "mofka_producer_flush_mode",
    "mofka_precreate_storage_provider",
    "mofka_group_ping_timeout_ms",
    "mofka_group_ping_interval_min_ms",
    "mofka_group_ping_interval_max_ms",
    "mofka_group_ping_max_timeouts",
    "kafka_acks",
]

OBSERVABILITY_FIELDS = [
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
    "client_tail_rpc_buffer_alloc_us_avg",
    "client_tail_rpc_bulk_expose_us_avg",
    "client_tail_rpc_payload_move_us_avg",
    "client_tail_rpc_total_us_avg",
    "client_replay_request_batch_count_avg",
    "client_replay_request_batch_count_max",
    "client_replay_request_batch_count_p95",
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
]


def rel(path: Path) -> str:
    return str(path.relative_to(REPO_ROOT))


def load_metrics(path_text: str) -> dict[str, Any]:
    path = REPO_ROOT / path_text
    metrics = json.loads(path.read_text())
    missing = [key for key in REQUIRED_COUNTS if key not in metrics]
    if missing:
        raise ValueError(f"{path_text} missing required count fields: {', '.join(missing)}")
    alias_pairs = (
        ("operation_count_per_client", "message_count_per_client"),
        ("operation_count_per_client", "messages_per_client"),
        ("total_operation_count", "total_message_count"),
        ("total_operation_count", "total_messages"),
        ("client_count", "parallel_client_count"),
        ("client_count", "parallel_clients"),
        ("node_count", "nodes"),
    )
    for canonical, alias in alias_pairs:
        if int(metrics[canonical]) != int(metrics[alias]):
            raise ValueError(
                f"{path_text} has inconsistent count fields: {alias}={metrics[alias]} "
                f"does not match {canonical}={metrics[canonical]}"
            )
    expected_total = int(metrics["operation_count_per_client"]) * int(metrics["client_count"])
    if int(metrics["total_operation_count"]) != expected_total:
        raise ValueError(
            f"{path_text} has inconsistent total_operation_count={metrics['total_operation_count']} "
            f"for operation_count_per_client*client_count={expected_total}"
        )
    expected_payload = int(metrics["message_size_bytes"]) * int(metrics["total_message_count"])
    if int(metrics["total_payload_bytes"]) != expected_payload:
        raise ValueError(
            f"{path_text} has inconsistent total_payload_bytes={metrics['total_payload_bytes']} "
            f"for message_size_bytes*total_message_count={expected_payload}"
        )
    return metrics


def row(path_text: str, *, throughput_field: str = "throughput_ops_per_sec") -> dict[str, Any]:
    metrics = load_metrics(path_text)
    total_messages = float(metrics.get("total_message_count") or metrics.get("total_operation_count") or 0)
    for seconds_field, derived_field in (
        ("archive_publication_confirm_seconds", "archive_publication_throughput_ops_per_sec"),
        ("archive_count_validation_seconds", "archive_count_validation_throughput_ops_per_sec"),
        ("archive_event_count_confirm_seconds", "archive_event_count_confirm_throughput_ops_per_sec"),
    ):
        seconds = metrics.get(seconds_field)
        if metrics.get(derived_field) is None and seconds:
            try:
                metrics[derived_field] = total_messages / float(seconds)
            except (TypeError, ValueError, ZeroDivisionError):
                pass
    throughput = metrics.get(throughput_field)
    if throughput is None:
        raise ValueError(f"{path_text} missing throughput field {throughput_field}")
    output = {key: metrics[key] for key in REQUIRED_COUNTS}
    output.update(
        {
            "duration_seconds": metrics.get("duration_seconds"),
            "throughput_ops_per_sec": throughput,
            "measurement_throughput_field": throughput_field,
            "source_throughput_ops_per_sec": metrics.get("throughput_ops_per_sec"),
            "success": metrics.get("success"),
            "semantic_boundary": metrics.get("semantic_boundary"),
            "append_ack_boundary": metrics.get("append_ack_boundary"),
            "durability_boundary": metrics.get("durability_boundary"),
            "producer_wait_mode": metrics.get("producer_wait_mode"),
            "producer_flush_mode": metrics.get("producer_flush_mode"),
            "storage_backend": metrics.get("storage_backend"),
            "read_path": metrics.get("read_path"),
            "chronolog_completion_mode": metrics.get("chronolog_completion_mode"),
            "throughput_semantics": metrics.get("throughput_semantics"),
            "latency_semantics": metrics.get("latency_semantics"),
            "metrics_path": path_text,
        }
    )
    if str(metrics.get("keeper_journal_durable_complete_before_publish") or "0") == "1":
        output["append_ack_boundary"] = (
            "deferred_rpc_response_after_keeper_journal_group_commit_fdatasync_before_tail_publish"
        )
        output["semantic_notes"] = (
            "RPC completion is allowed after Keeper journal grouped fdatasync and before Keeper tail-index "
            "publication. This is WAL durability evidence; immediate live-tail visibility is a separate "
            "post-ack publication step."
        )
    if metrics.get("chronolog_client_batch_keeper_selection") == "single_keeper":
        note = (
            "ChronoLog client batch Keeper selection is single_keeper: a multi-record client batch is "
            "routed to the Keeper selected from the first event timestamp. This is a batch-owned routing "
            "semantic and must not be treated as equivalent to per-event Keeper ownership."
        )
        if output.get("semantic_notes"):
            output["semantic_notes"] = f"{output['semantic_notes']} {note}"
        else:
            output["semantic_notes"] = note
    for key in (
        "workflow_total_message_throughput_ops_per_sec",
        "archive_publication_throughput_ops_per_sec",
        "archive_count_validation_throughput_ops_per_sec",
        "archive_event_count_confirm_throughput_ops_per_sec",
        "range_readback_throughput_ops_per_sec",
        "range_readback_duration_seconds",
        "archive_readback_mode",
        "range_readback_story_seconds_sum",
        "range_readback_story_seconds_max",
        "range_readback_event_count",
        "archive_event_count",
        "retrieved_event_count",
        "expected_retrieved_event_count",
        "append_throughput_ops_per_sec",
        "replay_throughput_ops_per_sec",
        "append_wall_throughput_ops_per_sec",
        "tail_read_count",
        "tail_success_count",
        "tail_max_retrieved_count",
        "tail_final_retrieved_count",
        "tail_avg_latency_ms",
        "tail_p50_latency_ms",
        "tail_p95_latency_ms",
        "tail_p99_latency_ms",
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
        "archive_publication_confirm_seconds",
        "archive_publication_throughput_ops_per_sec",
        "archive_publication_story_count",
        "archive_publication_file_count",
        "archive_count_validation_seconds",
        "archive_count_validation_throughput_ops_per_sec",
        "archive_publication_to_count_validation_seconds",
        "archive_event_count_confirm_seconds",
        "archive_event_count_confirm_throughput_ops_per_sec",
        "readback_event_count",
        "archive_readback_mode",
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
        "grapher_hdf5_open_us_max",
        "grapher_hdf5_rename_us_max",
        "grapher_hdf5_publish_rename_us_max",
        "grapher_hdf5_archive_manifest_write_us_max",
    ):
        if key in metrics:
            output[key] = metrics[key]
    for key in SEMANTIC_KNOBS:
        if key in metrics:
            output[key] = metrics[key]
    if metrics.get("system") == "chronolog" and not output.get("storage_backend"):
        durability = str(output.get("durability_boundary") or "")
        if durability == "not_proven_durable":
            output["storage_backend"] = "not_proven_durable"
        elif "keeper_local_journal" in durability:
            output["storage_backend"] = "chronolog_keeper_local_journal"
    if (
        metrics.get("workflow") == "mixed_append_tail"
        and not output.get("chronolog_mixed_tail_writer_release_story")
        and "afterreader" in path_text
    ):
        output["chronolog_mixed_tail_writer_release_story"] = "after_reader"
    tail_batch_max_bytes = str(output.get("keeper_tail_batch_max_bytes") or "").strip()
    tail_batch_max_events = str(output.get("keeper_tail_batch_max_events") or "").strip()
    if (
        metrics.get("system") == "chronolog"
        and metrics.get("workflow") == "mixed_append_tail"
        and str(metrics.get("tail_read_mode") or output.get("chronolog_mixed_tail_read_mode") or "")
        in {"keeper_cursor", "keeper_cursor_packed"}
        and (tail_batch_max_bytes not in {"", "0"} or tail_batch_max_events not in {"", "0"})
    ):
        suffix_parts: list[str] = []
        if tail_batch_max_bytes not in {"", "0"}:
            try:
                max_bytes = int(tail_batch_max_bytes)
            except ValueError:
                suffix_parts.append(f"{tail_batch_max_bytes}B")
            else:
                if max_bytes % (1024 * 1024) == 0:
                    suffix_parts.append(f"{max_bytes // (1024 * 1024)}MiB")
                else:
                    suffix_parts.append(f"{max_bytes}B")
        if tail_batch_max_events not in {"", "0"}:
            suffix_parts.append(f"{tail_batch_max_events}events")
        suffix = "_".join(suffix_parts) or "bounded"
        tail_mode = str(metrics.get("tail_read_mode") or output.get("chronolog_mixed_tail_read_mode") or "")
        default_boundary = (
            "append_then_keeper_journal_tail_catchup_keeper_cursor_packed_api"
            if tail_mode == "keeper_cursor_packed"
            else "append_then_keeper_journal_tail_catchup_keeper_cursor"
        )
        boundary = output.get("semantic_boundary") or default_boundary
        if "bounded_tail_batch" not in str(boundary):
            output["semantic_boundary"] = f"{boundary}_bounded_tail_batch_{suffix}"
        note = (
            "Keeper cursor tail retrieval used bounded per-Keeper journal-tail RPC batches "
            f"(max_events={tail_batch_max_events or '0'}, max_bytes={tail_batch_max_bytes or '0'}), "
            "so it is a streaming/cursor catch-up semantic and must not be merged with full ReplayStory rows."
        )
        if output.get("semantic_notes"):
            output["semantic_notes"] = f"{output['semantic_notes']} {note}"
        else:
            output["semantic_notes"] = note
    for key in OBSERVABILITY_FIELDS:
        if key in metrics:
            output[key] = metrics[key]
    if (
        metrics.get("system") == "mofka"
        and metrics.get("workflow") == "range_retrieval"
        and not output.get("semantic_boundary")
        and "memory" in path_text
    ):
        output.update(
            {
                "semantic_boundary": "producer_after_loop_then_memory_consumer_pull_catchup",
                "append_ack_boundary": "producer_push_deferred_wait_after_loop_no_explicit_flush",
                "durability_boundary": "mofka_memory_partition_not_durable_storage",
                "read_path": "mofka_consumer_pull_after_append",
                "storage_backend": "mofka_memory_partition",
                "semantic_notes": (
                    "Historical Mofka memory range row predates semantic fields in metrics.json; "
                    "manifest infers the labels from the cell path and keeps it memory/live only."
                ),
            }
        )
    return output


def sized(paths: dict[int, str], *, throughput_field: str = "throughput_ops_per_sec") -> dict[str, dict[str, Any]]:
    return {str(size): row(path, throughput_field=throughput_field) for size, path in paths.items()}


def blocked(cell: str, reason: str, evidence: list[str]) -> dict[str, Any]:
    return {
        "cell": cell,
        "status": "blocked",
        "reason": reason,
        "evidence": evidence,
    }


def accepted(
    cell: str,
    *,
    notes: str,
    paths: dict[int, str],
    throughput_field: str = "throughput_ops_per_sec",
) -> dict[str, Any]:
    rows = sized(paths, throughput_field=throughput_field)
    return {
        "cell": cell,
        "status": "accepted",
        "notes": notes,
        "rows": rows,
    }


def build_manifest() -> dict[str, Any]:
    return {
        "timestamp": datetime.now().astimezone().strftime("%Y-%m-%d %H:%M %Z"),
        "purpose": "Machine-readable six-way matrix status with explicit benchmark semantics.",
        "rules": {
            "chronolog_is_only_modifiable_target": True,
            "kafka_fixed_baseline_only": True,
            "mofka_fixed_baseline_only": True,
            "memory_live_and_storage_durable_are_separate": True,
            "small_100_or_1000_event_runs_are_smoke_only": True,
            "required_count_fields": REQUIRED_COUNTS,
            "required_message_dimension_fields": [
                "node_count",
                "nodes",
                "parallel_client_count",
                "parallel_clients",
                "message_size_bytes",
                "message_count_per_client",
                "messages_per_client",
                "total_message_count",
                "total_messages",
                "total_payload_bytes",
            ],
            "archive_storage_workflow_throughput_field": "workflow_total_message_throughput_ops_per_sec",
            "archive_storage_readback_throughput_field": "range_readback_throughput_ops_per_sec",
        },
        "workloads": {
            "append_throughput": {
                "sizes": [1024, 65536],
                "cells": [
                    accepted(
                        "chronolog_live_memory_append",
                        notes="Live-return append acknowledgement only; not durable/storage evidence.",
                        paths={
                            1024: ".agent/results/20260516-161000-grapher-orphan-shutdown-live-1k/001-chronolog-append_throughput-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/chronolog_live_memory_append-n4-c4-s65536-o10000/001-chronolog-append_throughput-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail",
                        notes="Strict per-event Keeper-local journal fdatasync/restart-recovery tail evidence; not Grapher archive storage.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/chronolog_journal_durable_restart_tail-n4-c4-s1024-o40000/001-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/chronolog_journal_durable_restart_tail-n4-c4-s65536-o10000/001-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_outstanding64",
                        notes="Keeper-local journal fdatasync/restart-recovery tail evidence with 64 bounded in-flight appends per client; this is a distinct durable wait semantic that allows group commit batching.",
                        paths={
                            1024: ".agent/results/20260516-183500-chronolog-1k-journal-outstanding64/001-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-181000-chronolog-64k-journal-outstanding64/001-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_afterloop_batch64",
                        notes="Keeper-local journal fdatasync/restart-recovery tail evidence with async 64-record client batches and after-loop wait; explicit durable wait semantic, not a default.",
                        paths={
                            1024: ".agent/results/20260516-192000-chronolog-1k-journal-afterloop-batch64/001-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-191000-chronolog-64k-journal-afterloop-batch64/001-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_afterloop_batch16",
                        notes="Keeper-local journal fdatasync/restart-recovery tail evidence with async 16-record client batches and after-loop wait; current best 64KiB candidate, not a default.",
                        paths={
                            1024: ".agent/results/20260516-202000-chronolog-1k-afterloop-batch16-guardrail/001-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-201000-chronolog-64k-afterloop-batch16-repeat/001-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_bounded16_batch16",
                        notes=(
                            "Keeper-local journal fdatasync/restart-recovery tail evidence with 16-record "
                            "client batches and bounded outstanding window 16; profile-driven backpressure "
                            "candidate. The 64KiB manifest row uses the conservative repeat trial."
                        ),
                        paths={
                            1024: ".agent/results/20260516-223000-chronolog-1k-outstanding16-guardrail/001-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-221500-chronolog-64k-outstanding16-repeat/001-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t2/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_bounded16_batch16_move_payloads",
                        notes=(
                            "Keeper-local journal fdatasync/restart-recovery tail evidence with 16-record "
                            "client batches, bounded outstanding window 16, and Keeper journal batch payload "
                            "move enabled. This is the copy-reduced durable wait semantic; the 64KiB manifest "
                            "row uses the latest accepted async batch completion retest."
                        ),
                        paths={
                            1024: ".agent/results/20260517-111500-chronolog-1k-shards4-current/001-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260517-101500-chronolog-64k-async-batch-current-ab/002-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_local_journal_size_sensitive",
                        notes=(
                            "Append-only Keeper-local journal group-commit fdatasync evidence comparing shared "
                            "NFS result-dir journal placement against node-local `/mnt/nvme` Keeper WAL placement. "
                            "This is a paper-aligned fast-storage placement candidate, not restart-tail evidence "
                            "and not a global default: the 64KiB row improves strongly, while the 1KiB guardrail "
                            "regresses. Keep `keeper_journal_placement`, `keeper_journal_local_base`, message size, "
                            "and total data volume explicit before comparing."
                        ),
                        paths={
                            1024: ".agent/results/20260517-011500-chronolog-1k-journal-placement-guardrail/002-chronolog-append_throughput-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260517-010500-chronolog-64k-journal-placement-shared-vs-nvme/002-chronolog-append_throughput-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_scaling_2n",
                        notes=(
                            "Node-count scaling coverage for the append-only Keeper-local journal group-commit "
                            "fdatasync semantic. This 2-node row uses 4 clients, 40000 messages per client, "
                            "1024-byte messages, bounded_outstanding producer 16/batch 16, shared Keeper "
                            "journal placement, group_commit_flush_events=64, batch_writev=1, "
                            "move_batch_payloads=1, and async batch-completion dispatch. Accept as graph-ready "
                            "ChronoLog scaling evidence only; do not compare against 4-node Kafka/Mofka rows "
                            "or different ChronoLog durable semantics without matching the full workload."
                        ),
                        paths={
                            1024: ".agent/results/20260518-012000-node-scaling-2n-chronolog-durable-1k/001-chronolog-append_throughput-n2-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260518-021000-node-scaling-2n-chronolog-durable-64k/001-chronolog-append_throughput-n2-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_scaling_8n",
                        notes=(
                            "8-node scaling coverage for the append-only Keeper-local journal group-commit "
                            "fdatasync semantic. The 1KiB row uses 4 clients, 40000 messages per client, "
                            "and 160000 total messages; the 64KiB row uses 4 clients, 10000 messages per "
                            "client, and 40000 total messages. Both use bounded_outstanding producer 16 "
                            "and the accepted deferred-tail-only Keeper journal completion path. Accept as "
                            "graph-ready ChronoLog 8-node evidence only; storage-vs-storage conclusions must "
                            "still preserve the Mofka PMDK provider and ack-boundary caveats."
                        ),
                        paths={
                            1024: ".agent/results/20260518-151000-8n-chronolog-append-1k-jqwrapper/001-chronolog-append_throughput-n8-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260518-163500-8n-chronolog-append-64k/001-chronolog-append_throughput-n8-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_local_journal_owned_batch_threshold",
                        notes=(
                            "Append-only Keeper-local journal group-commit fdatasync evidence with explicit "
                            "chrono-bench owned-batch transfer gated by message size. The threshold is recorded "
                            "as chronolog_bench_owned_batch_min_message_size=4096: 1KiB stays on the normal "
                            "const-reference path, while 64KiB uses the ownership-moving batch path. This is "
                            "a default-off size-sensitive diagnostic semantic, not a global ChronoLog default."
                        ),
                        paths={
                            1024: ".agent/results/20260517-112000-owned-batch-threshold-1k-guardrail/chronolog/metrics.json",
                            65536: ".agent/results/20260517-113000-owned-batch-threshold-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_local_journal_notify_owner_empty",
                        notes=(
                            "Append-only Keeper-local journal group-commit fdatasync evidence with "
                            "keeper_journal_notify_owner_only_on_empty=1. This coalesces shard-owner condition "
                            "variable notifications so enqueue only wakes the owner on empty-to-nonempty queue "
                            "transitions. It is a bounded scheduling semantic, not a durability change and not "
                            "a global default."
                        ),
                        paths={
                            1024: ".agent/results/20260517-102000-notify-owner-empty-1k-guardrail/chronolog/metrics.json",
                            65536: ".agent/results/20260517-101000-notify-owner-empty-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_local_journal_notify_owner_outstanding64",
                        notes=(
                            "Append-only Keeper-local journal group-commit fdatasync evidence with "
                            "keeper_journal_notify_owner_only_on_empty=1 and chronolog_producer_outstanding=64. "
                            "This is a throughput-oriented in-flight RPC semantic motivated by the 1KiB "
                            "gperftools wait/progress profile. It improves throughput at both 1KiB and 64KiB, "
                            "but increases RPC completion wait, owner queue wait, and queue depth; do not report "
                            "it as a latency improvement or a global default."
                        ),
                        paths={
                            1024: ".agent/results/20260517-123500-notify-owner-empty-1k-outstanding64/chronolog/metrics.json",
                            65536: ".agent/results/20260517-124500-notify-owner-empty-64k-outstanding64/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_local_journal_notify_owner_bounded_futures64",
                        notes=(
                            "Append-only Keeper-local journal group-commit fdatasync evidence with "
                            "keeper_journal_notify_owner_only_on_empty=1, chronolog_producer_outstanding=64, "
                            "chronolog_producer_wait_policy=bounded_futures, and "
                            "chronolog_bench_bound_keeper_futures=1. This corrected harness semantic bounds "
                            "pending Keeper RPC futures instead of submitted composite write calls. The 1KiB "
                            "row remains positive versus notify-owner-only, while the 64KiB row is a guardrail "
                            "and is not a broad throughput improvement versus notify-owner-only. Report it as "
                            "an explicit wait-boundary semantic, not a global default."
                        ),
                        paths={
                            1024: ".agent/results/20260517-124000-bound-keeper-futures-1k/chronolog/metrics.json",
                            65536: ".agent/results/20260517-124500-bound-keeper-futures-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_local_journal_notify_owner_bounded_futures64_cap128",
                        notes=(
                            "Append-only Keeper-local journal group-commit fdatasync evidence with "
                            "keeper_journal_notify_owner_only_on_empty=1, chronolog_producer_outstanding=64, "
                            "chronolog_producer_wait_policy=bounded_futures, chronolog_bench_bound_keeper_futures=1, "
                            "keeper_journal_group_commit_flush_events=128, and "
                            "keeper_journal_group_commit_strict_flush_event_cap=1. This is an explicit larger "
                            "group-commit batching semantic that waits for Keeper-local fdatasync before durable "
                            "publish; it should be compared separately from cap64 latency/ack-boundary rows and "
                            "is not archive/storage evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260517-132500-bound-keeper-futures-1k-strict-cap128-guardrail/chronolog/metrics.json",
                            4096: ".agent/results/20260517-133500-bound-keeper-futures-4k-strict-cap128/chronolog/metrics.json",
                            16384: ".agent/results/20260517-134000-bound-keeper-futures-16k-strict-cap128/chronolog/metrics.json",
                            65536: ".agent/results/20260517-132000-bound-keeper-futures-64k-strict-cap128/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_local_journal_cap128_wal_ack_before_tail_publish",
                        notes=(
                            "Append-only Keeper-local WAL durable acknowledgment before tail-index publication. "
                            "This uses node-local `/mnt/nvme` Keeper journal placement, "
                            "keeper_journal_group_commit_flush_events=128, strict flush cap, "
                            "keeper_journal_durable_complete_before_publish=1, "
                            "chronolog_producer_wait_policy=bounded_futures, and "
                            "chronolog_bench_bound_keeper_futures=1. The append response remains behind "
                            "Keeper-local group fdatasync, but tail-index publication and immediate live-tail "
                            "visibility are explicitly outside the append acknowledgment boundary. Report this "
                            "as Keeper-local WAL durability only, not archive completion and not immediate "
                            "tail visibility evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260517-151500-cap128-1k-durable-before-tail-publish-guardrail/chronolog/metrics.json",
                            65536: ".agent/results/20260517-150500-cap128-64k-durable-before-tail-publish/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_batch_owned_single_keeper",
                        notes=(
                            "Keeper-local journal fdatasync/restart-recovery tail evidence with 16-record "
                            "client batches, bounded outstanding window 16, Keeper journal batch payload "
                            "move enabled, async batch completion enabled, and client batch routing set to "
                            "single_keeper. This is a promising counterfactual and explicit batch-owned "
                            "routing semantic: the whole batch is routed to the Keeper selected from the "
                            "first event timestamp. It is not a default and must not be compared as "
                            "per-event Keeper ownership."
                        ),
                        paths={
                            1024: ".agent/results/20260517-130000-chronolog-1k-batch-keeper-selection-guardrail/002-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260517-123000-chronolog-64k-batch-keeper-selection-ab/002-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_per_event_batch64",
                        notes=(
                            "Keeper-local journal fdatasync/restart-recovery tail evidence with per-event "
                            "Keeper routing preserved, 64-record client batches, bounded outstanding window "
                            "16, Keeper journal batch payload move enabled, and async batch completion enabled. "
                            "This is a throughput-oriented coalescing knob with a 1KiB latency caveat; it is "
                            "not a default promotion."
                        ),
                        paths={
                            1024: ".agent/results/20260517-141000-chronolog-1k-per-event-batch-size-guardrail/001-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260517-134500-chronolog-64k-per-event-batch-size-sweep/002-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_time_bucket_1ms",
                        notes=(
                            "Keeper-local journal fdatasync/restart-recovery tail evidence with per-event "
                            "Keeper routing using a 1 ms timestamp bucket before RoundRobin Keeper selection. "
                            "This is a default-off time-bucket ownership semantic and coalescing knob. It reduced "
                            "future fanout and improved 1KiB throughput, while the 64KiB row traded lower p99 for "
                            "lower throughput."
                        ),
                        paths={
                            1024: ".agent/results/20260516-222500-chronolog-1k-time-bucket-keeper-guardrail/002-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-220500-chronolog-64k-time-bucket-keeper-ab/002-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_time_bucket_250us",
                        notes=(
                            "Keeper-local journal fdatasync/restart-recovery tail evidence with per-event "
                            "Keeper routing using a 250 us timestamp bucket before RoundRobin Keeper selection. "
                            "This is a default-off cross-size time-bucket candidate: it improved the matched "
                            "1KiB guardrail versus raw timestamp routing and was the best 64KiB bucket point "
                            "in the 100us/250us/500us/2ms sweep. A later repeat attempt was incomplete and "
                            "mixed, so this row must stay caveated and must not be treated as a stable "
                            "performance claim or runtime default."
                        ),
                        paths={
                            1024: ".agent/results/20260516-223600-chronolog-1k-time-bucket-250us-guardrail/002-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-225000-chronolog-64k-time-bucket-sweep/002-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_bounded_futures_diagnostic",
                        notes=(
                            "Keeper-local journal fdatasync/restart-recovery tail evidence for the diagnostic "
                            "bounded_futures wait policy. This bounds pending Keeper RPC futures rather than "
                            "logical client submit futures. The 64KiB A/B accepted it as a diagnostic "
                            "backpressure knob but rejected it for default promotion because throughput and "
                            "p99 regressed despite lower owner/RPC queue pressure."
                        ),
                        paths={
                            65536: ".agent/results/20260517-151500-chronolog-64k-bounded-futures-ab/001-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_wal_drain_async_workers1",
                        notes=(
                            "Append WAL-drain compatibility semantic with Keeper-local journal fdatasync, "
                            "WAL-drain readback into the Keeper timeline, bounded outstanding window 16, "
                            "16-record client batches, 4 Keeper journal shards, and one async-drain worker. "
                            "This is append WAL-drain evidence only: it is not restart-recovery tail evidence "
                            "and not Grapher archive/storage readback."
                        ),
                        paths={
                            1024: ".agent/results/20260517-173500-chronolog-async-drain-threads-nonsmoke-1k/001-chronolog-append_throughput-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260517-174500-chronolog-async-drain-threads-nonsmoke-64k/001-chronolog-append_throughput-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_wal_drain_async_workers4",
                        notes=(
                            "Append WAL-drain compatibility semantic with Keeper-local journal fdatasync, "
                            "WAL-drain readback into the Keeper timeline, bounded outstanding window 16, "
                            "16-record client batches, 4 Keeper journal shards, and four async-drain workers. "
                            "Non-smoke validation is size-sensitive: 4 workers reduced queue depth but hurt "
                            "1KiB throughput, while improving 64KiB throughput. This is not a global default, "
                            "not restart-recovery tail evidence, and not Grapher archive/storage readback."
                        ),
                        paths={
                            1024: ".agent/results/20260517-173500-chronolog-async-drain-threads-nonsmoke-1k/002-chronolog-append_throughput-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260517-174500-chronolog-async-drain-threads-nonsmoke-64k/002-chronolog-append_throughput-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_journal_group_commit_size_aware_queue_boundary",
                        notes=(
                            "Append-only Keeper-local journal grouped fdatasync evidence with deferred RPC "
                            "completion, bounded_outstanding window 16, 16-record client batches, 4 Keeper "
                            "journal shards, async batch-completion dispatch, and a default-off size-aware "
                            "queue-boundary minimum-record wait policy. This is Keeper journal durability "
                            "evidence but not restart-recovery tail evidence and not Grapher archive/storage "
                            "readback. The 1KiB row is the small-message guardrail; the 64KiB row uses the "
                            "accepted repeat."
                        ),
                        paths={
                            1024: ".agent/results/20260517-083000-chronolog-1k-size-aware-queue-boundary-guardrail/chronolog/metrics.json",
                            65536: ".agent/results/20260517-085000-chronolog-64k-size-aware-queue-boundary-repeat/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_before_tail_publish",
                        notes=(
                            "Keeper-local grouped journal fdatasync with deferred RPC completion before "
                            "tail-index publication; valid Keeper-journal durability boundary, distinct from "
                            "the live-tail-published append semantic, and rejected as a default performance direction."
                        ),
                        paths={
                            65536: ".agent/results/20260517-050000-chronolog-64k-durable-before-publish-rerun/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_append",
                        notes="Kafka fixed baseline with producer acks=0; no broker acknowledgement durability boundary.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/kafka_acks0_append-n4-c4-s1024-o40000/001-kafka-append_throughput-n4-c4-s1024-o40000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/kafka_acks0_append-n4-c4-s65536-o10000/001-kafka-append_throughput-n4-c4-s65536-o10000-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_append_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Kafka fixed baseline with producer acks=0; "
                            "no broker acknowledgement durability boundary. This row matches the 2-node "
                            "ChronoLog scaling workload shape but must not be merged with durable/storage "
                            "semantics."
                        ),
                        paths={
                            1024: ".agent/results/20260518-014000-node-scaling-2n-kafka-1k/001-kafka-append_throughput-n2-c4-s1024-o40000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-021500-node-scaling-2n-kafka-64k/001-kafka-append_throughput-n2-c4-s65536-o10000-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_append_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Kafka fixed baseline with producer acks=0; "
                            "no broker acknowledgement durability boundary. The 1KiB row uses 4 clients, "
                            "40000 messages per client, and 160000 total messages; the 64KiB row uses "
                            "4 clients, 10000 messages per client, and 40000 total messages. Memory/no-ack "
                            "style evidence only; do not merge with durable/storage semantics."
                        ),
                        paths={
                            1024: ".agent/results/20260518-152500-8n-fixed-baselines-append-1k/002-kafka-append_throughput-n8-c4-s1024-o40000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-170000-8n-fixed-baselines-append-64k/002-kafka-append_throughput-n8-c4-s65536-o10000-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_append_rf1",
                        notes="Kafka fixed baseline with acks=all and replication factor 1.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/kafka_acksall_append_rf1_retry-n4-c4-s1024-o40000/001-kafka-append_throughput-n4-c4-s1024-o40000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/kafka_acksall_append_rf1-n4-c4-s65536-o10000/001-kafka-append_throughput-n4-c4-s65536-o10000-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_append_rf1_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Kafka fixed baseline with acks=all and replication "
                            "factor 1. Treat this as the explicit leader/in-sync-replica acknowledgement "
                            "boundary for this harness, not as a multi-replica durability claim."
                        ),
                        paths={
                            1024: ".agent/results/20260518-014000-node-scaling-2n-kafka-1k/002-kafka-append_throughput-n2-c4-s1024-o40000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-021500-node-scaling-2n-kafka-64k/002-kafka-append_throughput-n2-c4-s65536-o10000-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_append_rf1_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Kafka fixed baseline with acks=all and replication "
                            "factor 1. The 1KiB row uses 4 clients, 40000 messages per client, and 160000 "
                            "total messages; the 64KiB row uses 4 clients, 10000 messages per client, and "
                            "40000 total messages. Treat as leader/in-sync-replica acknowledgement boundary "
                            "for this harness, not as a multi-replica durability claim."
                        ),
                        paths={
                            1024: ".agent/results/20260518-152500-8n-fixed-baselines-append-1k/003-kafka-append_throughput-n8-c4-s1024-o40000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-170000-8n-fixed-baselines-append-64k/003-kafka-append_throughput-n8-c4-s65536-o10000-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_nowait_noflush_append",
                        notes="Mofka memory partition, push without explicit wait and no explicit producer flush.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/mofka_memory_nowait_noflush_append-n4-c4-s1024-o40000/001-mofka-append_throughput-memory-memory-none-no_flush-n4-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/mofka_memory_nowait_noflush_retry-n4-c4-s65536-o10000/001-mofka-append_throughput-memory-memory-none-no_flush-n4-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_nowait_noflush_append_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Mofka memory partition with no explicit producer wait "
                            "and no explicit flush. Memory/live evidence only; not durable storage evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260518-014500-node-scaling-2n-mofka-memory-1k/001-mofka-append_throughput-memory-memory-none-no_flush-n2-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-022000-node-scaling-2n-mofka-memory-nowait-64k/001-mofka-append_throughput-memory-memory-none-no_flush-n2-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_nowait_noflush_append_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Mofka memory partition with no explicit producer "
                            "wait and no explicit flush. The 1KiB row uses 4 clients, 40000 messages per "
                            "client, and 160000 total messages; the 64KiB row uses 4 clients, 10000 messages "
                            "per client, and 40000 total messages. Memory/live evidence only; not durable "
                            "storage evidence and not a substitute for PMDK rows."
                        ),
                        paths={
                            1024: ".agent/results/20260518-152500-8n-fixed-baselines-append-1k/004-mofka-append_throughput-memory-memory-none-no_flush-n8-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-170000-8n-fixed-baselines-append-64k/004-mofka-append_throughput-memory-memory-none-no_flush-n8-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_wait_flush_append",
                        notes="Mofka memory partition, per-event producer wait and flush-after-loop; memory/live evidence only.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/mofka_memory_wait_flush_append-n4-c4-s1024-o40000/001-mofka-append_throughput-memory-memory-per_event-after_loop-n4-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/mofka_memory_wait_flush_append-n4-c4-s65536-o10000/001-mofka-append_throughput-memory-memory-per_event-after_loop-n4-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_wait_flush_append_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Mofka memory partition with per-event producer wait "
                            "and flush after loop. Memory/live evidence only; not durable storage evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260518-014500-node-scaling-2n-mofka-memory-1k/004-mofka-append_throughput-memory-memory-per_event-after_loop-n2-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-022500-node-scaling-2n-mofka-memory-waitflush-64k/001-mofka-append_throughput-memory-memory-per_event-after_loop-n2-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_wait_flush_append_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Mofka memory partition with per-event producer wait "
                            "and flush after loop. The 1KiB row uses 4 clients, 40000 messages per client, "
                            "and 160000 total messages; the 64KiB row uses 4 clients, 10000 messages per "
                            "client, and 40000 total messages. Memory/live evidence only; not durable "
                            "storage evidence and not a substitute for PMDK rows."
                        ),
                        paths={
                            1024: ".agent/results/20260518-152500-8n-fixed-baselines-append-1k/005-mofka-append_throughput-memory-memory-per_event-after_loop-n8-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-170000-8n-fixed-baselines-append-64k/005b-mofka-append_throughput-memory-memory-per_event-after_loop-n8-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_nowait_noflush_append",
                        notes=(
                            "Mofka default partition with precreated PMDK Warabi storage target, push without "
                            "explicit wait and no explicit flush. This is storage-backed placement evidence, "
                            "not synchronous completion evidence. The 64KiB row uses explicit Flock group-timeout "
                            "settings to keep live storage daemons from being evicted during large PMDK startup."
                        ),
                        paths={
                            1024: ".agent/results/20260516-150500-mofka-pmdk-storage-nowait-1k/001-mofka-append_throughput-default-pmdk-none-no_flush-n4-c4-s1024-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260516-154000-mofka-pmdk-64k-group-timeout-probe/001-mofka-append_throughput-default-pmdk-none-no_flush-n4-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_nowait_noflush_append_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Mofka default partition with precreated PMDK Warabi "
                            "storage target, no explicit producer wait, and no explicit flush. Storage-backed "
                            "placement evidence, not synchronous completion evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260518-015000-node-scaling-2n-mofka-pmdk-nowait-1k/001-mofka-append_throughput-default-pmdk-none-no_flush-n2-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-023000-node-scaling-2n-mofka-pmdk-nowait-64k/001-mofka-append_throughput-default-pmdk-none-no_flush-n2-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_nowait_noflush_append_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Mofka default partition with precreated PMDK Warabi "
                            "storage target for the 1KiB row and dynamic PMDK provider creation for the 64KiB "
                            "row. The 1KiB row uses 512MiB PMDK pools because the original 64MiB-pool row "
                            "plateaued around event index 14460 per client, consistent with underprovisioned "
                            "PMDK capacity for 160000 total 1024-byte messages plus metadata. The 64KiB row "
                            "uses one dynamic 4GiB PMDK target after the precreated multi-provider 4GiB attempt "
                            "failed during startup. Storage-backed placement evidence, not synchronous completion "
                            "evidence; do not merge the precreated and dynamic provider startup semantics."
                        ),
                        paths={
                            1024: ".agent/results/20260518-160000-8n-mofka-pmdk-512m-capacity-probe/001-mofka-append_throughput-default-pmdk-none-no_flush-n8-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-170000-8n-fixed-baselines-append-64k/006b-mofka-append_throughput-default-pmdk-dynamic-none-no_flush-n8-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_afterloop_flush_append",
                        notes=(
                            "Mofka default partition with precreated PMDK Warabi storage target, producer wait "
                            "after the submission loop, and flush after the loop. This is deferred wait/flush "
                            "storage-backed evidence, not per-event wait evidence. The 64KiB row uses explicit "
                            "Flock group-timeout settings to keep live storage daemons from being evicted during "
                            "large PMDK startup."
                        ),
                        paths={
                            1024: ".agent/results/20260517-074000-mofka-pmdk-1k-afterloop-40000/mofka/metrics.json",
                            65536: ".agent/results/20260516-155000-mofka-pmdk-64k-afterloop-group-timeout/001-mofka-append_throughput-default-pmdk-after_loop-after_loop-n4-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_afterloop_flush_append_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Mofka default partition with precreated PMDK Warabi "
                            "storage target, producer wait after the submission loop, and flush after the loop. "
                            "This is deferred wait/flush storage-backed evidence, not per-event wait evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260518-015500-node-scaling-2n-mofka-pmdk-afterloop-1k/001-mofka-append_throughput-default-pmdk-after_loop-after_loop-n2-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-023500-node-scaling-2n-mofka-pmdk-afterloop-64k/001-mofka-append_throughput-default-pmdk-after_loop-after_loop-n2-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_afterloop_flush_append_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Mofka default partition with precreated PMDK Warabi "
                            "storage target for the 1KiB row and dynamic PMDK provider creation for the 64KiB "
                            "row, with producer wait after the submission loop and flush after the loop. The "
                            "1KiB row uses 512MiB PMDK pools because the original 64MiB-pool row plateaued "
                            "around event index 14460 per client, consistent with underprovisioned PMDK capacity "
                            "for 160000 total 1024-byte messages plus metadata. The 64KiB row uses one dynamic "
                            "4GiB PMDK target after the precreated multi-provider 4GiB attempt failed during "
                            "startup. This is deferred wait/flush storage-backed evidence, not per-event wait "
                            "evidence; do not merge the precreated and dynamic provider startup semantics."
                        ),
                        paths={
                            1024: ".agent/results/20260518-160000-8n-mofka-pmdk-512m-capacity-probe/002-mofka-append_throughput-default-pmdk-after_loop-after_loop-n8-c4-s1024-o40000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-170000-8n-fixed-baselines-append-64k/007b-mofka-append_throughput-default-pmdk-dynamic-after_loop-after_loop-n8-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_per_event_wait_flush_append",
                        notes=(
                            "Mofka default partition with precreated PMDK Warabi storage target, producer "
                            "per-event wait, and flush after the loop. The 1KiB row uses relaxed Flock ping "
                            "settings and 3GiB PMDK targets; the 64KiB row uses relaxed Flock ping settings "
                            "and 4GiB PMDK targets."
                        ),
                        paths={
                            1024: ".agent/results/20260518-205732-mofka-pmdk-per-event-flush-1k-retry/mofka/metrics.json",
                            65536: ".agent/results/20260518-210314-mofka-pmdk-per-event-flush-64k-retry/mofka/metrics.json",
                        },
                    ),
                ],
            },
            "append_throughput_volume_sweep": {
                "sizes": [1024, 4096, 16384, 65536],
                "cells": [
                    accepted(
                        "chronolog_live_memory_append_volume",
                        notes=(
                            "ChronoLog live-return append acknowledgement across the payload-size sweep. "
                            "This is an append enqueue/record_event-return boundary and is not durable/storage "
                            "evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260516-161000-grapher-orphan-shutdown-live-1k/001-chronolog-append_throughput-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            4096: ".agent/results/20260517-085000-chronolog-live-volume-4k-16k/001-chronolog-append_throughput-n4-c4-s4096-o10000-t1/chronolog/metrics.json",
                            16384: ".agent/results/20260517-085000-chronolog-live-volume-4k-16k/002-chronolog-append_throughput-n4-c4-s16384-o10000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/chronolog_live_memory_append-n4-c4-s65536-o10000/001-chronolog-append_throughput-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_journal_durable_restart_tail_bounded16_batch16_move_payloads_volume",
                        notes=(
                            "ChronoLog current copy-reduced Keeper-local journal durable semantic across "
                            "payload sizes. This is Keeper-local journal group-commit fdatasync with deferred "
                            "RPC completion, bounded_outstanding window 16, batch size 16, Keeper restart, "
                            "and recovered Keeper-tail readback; not archive/storage evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260516-145500-chronolog-durable-volume-restart-probe/s1024-o40000/001-chronolog-keeper_restart_recovery-n4-c4-s1024-o40000-t1/chronolog/metrics.json",
                            4096: ".agent/results/20260516-145500-chronolog-durable-volume-restart-probe/s4096-o10000/001-chronolog-keeper_restart_recovery-n4-c4-s4096-o10000-t1/chronolog/metrics.json",
                            16384: ".agent/results/20260516-145500-chronolog-durable-volume-restart-probe/s16384-o10000/001-chronolog-keeper_restart_recovery-n4-c4-s16384-o10000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-145500-chronolog-durable-volume-restart-probe/s65536-o10000/001-chronolog-keeper_restart_recovery-n4-c4-s65536-o10000-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_append_durable_node_local_journal_notify_owner_bounded_futures64_volume",
                        notes=(
                            "ChronoLog Keeper-local journal group-commit fdatasync append evidence with "
                            "notify-owner-only-on-empty=1, producer_outstanding=64, producer_batch_size=16, "
                            "producer_wait_policy=bounded_futures, and corrected Keeper-future accounting. "
                            "All four rows carry chronolog_bench_bound_keeper_futures=1 and bound pending "
                            "Keeper RPC futures rather than composite write calls. This is not archive/storage "
                            "readback and should not be reported as a latency-default claim."
                        ),
                        paths={
                            1024: ".agent/results/20260517-124000-bound-keeper-futures-1k/chronolog/metrics.json",
                            4096: ".agent/results/20260517-125500-bound-keeper-futures-4k/chronolog/metrics.json",
                            16384: ".agent/results/20260517-130000-bound-keeper-futures-16k/chronolog/metrics.json",
                            65536: ".agent/results/20260517-124500-bound-keeper-futures-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_append_volume",
                        notes="Kafka fixed baseline with producer acks=0 across the same payload-size sweep where rows are available.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/kafka_acks0_append-n4-c4-s1024-o40000/001-kafka-append_throughput-n4-c4-s1024-o40000-t1/kafka/metrics.json",
                            4096: ".agent/results/20260516-144500-fixed-baseline-volume-gap/kafka_acks0_4k/001-kafka-append_throughput-n4-c4-s4096-o10000-t1/kafka/metrics.json",
                            16384: ".agent/results/20260516-144500-fixed-baseline-volume-gap/kafka_acks0_16k/001-kafka-append_throughput-n4-c4-s16384-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/kafka_acks0_append-n4-c4-s65536-o10000/001-kafka-append_throughput-n4-c4-s65536-o10000-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_append_rf1_volume",
                        notes="Kafka fixed baseline with acks=all and replication factor 1 across the same payload-size sweep where rows are available.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/kafka_acksall_append_rf1_retry-n4-c4-s1024-o40000/001-kafka-append_throughput-n4-c4-s1024-o40000-t1/kafka/metrics.json",
                            4096: ".agent/results/20260516-144500-fixed-baseline-volume-gap/kafka_acksall_4k_retry/001-kafka-append_throughput-n4-c4-s4096-o10000-t1/kafka/metrics.json",
                            16384: ".agent/results/20260516-144500-fixed-baseline-volume-gap/kafka_acksall_16k/001-kafka-append_throughput-n4-c4-s16384-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/kafka_acksall_append_rf1-n4-c4-s65536-o10000/001-kafka-append_throughput-n4-c4-s65536-o10000-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_nowait_noflush_append_volume",
                        notes="Mofka memory partition, push without explicit wait and no explicit producer flush across the same payload-size sweep where rows are available.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/mofka_memory_nowait_noflush_append-n4-c4-s1024-o40000/001-mofka-append_throughput-memory-memory-none-no_flush-n4-c4-s1024-o40000-t1/mofka/metrics.json",
                            4096: ".agent/results/20260516-144500-fixed-baseline-volume-gap/mofka_memory_none_4k/001-mofka-append_throughput-memory-memory-none-no_flush-n4-c4-s4096-o10000-t1/mofka/metrics.json",
                            16384: ".agent/results/20260516-144500-fixed-baseline-volume-gap/mofka_memory_none_16k/001-mofka-append_throughput-memory-memory-none-no_flush-n4-c4-s16384-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/mofka_memory_nowait_noflush_retry-n4-c4-s65536-o10000/001-mofka-append_throughput-memory-memory-none-no_flush-n4-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_wait_flush_append_volume",
                        notes="Mofka memory partition, per-event producer wait and flush-after-loop across the same payload-size sweep where rows are available. Memory/live evidence only.",
                        paths={
                            1024: ".agent/results/20260516-163000-append-sixway-1k/mofka_memory_wait_flush_append-n4-c4-s1024-o40000/001-mofka-append_throughput-memory-memory-per_event-after_loop-n4-c4-s1024-o40000-t1/mofka/metrics.json",
                            4096: ".agent/results/20260516-144500-fixed-baseline-volume-gap/mofka_memory_wait_4k/001-mofka-append_throughput-memory-memory-per_event-after_loop-n4-c4-s4096-o10000-t1/mofka/metrics.json",
                            16384: ".agent/results/20260516-144500-fixed-baseline-volume-gap/mofka_memory_wait_16k/001-mofka-append_throughput-memory-memory-per_event-after_loop-n4-c4-s16384-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260516-170000-append-sixway-64k/mofka_memory_wait_flush_append-n4-c4-s65536-o10000/001-mofka-append_throughput-memory-memory-per_event-after_loop-n4-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_nowait_noflush_append_volume",
                        notes=(
                            "Mofka default partition with precreated PMDK Warabi storage target, push without "
                            "explicit wait and no explicit flush across the payload-size sweep. Storage-backed "
                            "placement evidence, not synchronous completion evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260516-150500-mofka-pmdk-storage-nowait-1k/001-mofka-append_throughput-default-pmdk-none-no_flush-n4-c4-s1024-o10000-t1/mofka/metrics.json",
                            4096: ".agent/results/20260516-183500-mofka-pmdk-volume-nowait-fixed/001-mofka-append_throughput-default-pmdk-none-no_flush-n4-c4-s4096-o10000-t1/mofka/metrics.json",
                            16384: ".agent/results/20260516-183500-mofka-pmdk-volume-nowait-fixed/002-mofka-append_throughput-default-pmdk-none-no_flush-n4-c4-s16384-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260516-154000-mofka-pmdk-64k-group-timeout-probe/001-mofka-append_throughput-default-pmdk-none-no_flush-n4-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_pmdk_afterloop_flush_append_volume",
                        notes=(
                            "Mofka default partition with precreated PMDK Warabi storage target, producer wait "
                            "after the submission loop, and flush after the loop across the payload-size sweep. "
                            "Deferred wait/flush storage-backed evidence, not per-event wait evidence."
                        ),
                        paths={
                            1024: ".agent/results/20260517-074000-mofka-pmdk-1k-afterloop-40000/mofka/metrics.json",
                            4096: ".agent/results/20260516-184500-mofka-pmdk-volume-afterloop/001-mofka-append_throughput-default-pmdk-after_loop-after_loop-n4-c4-s4096-o10000-t1/mofka/metrics.json",
                            16384: ".agent/results/20260516-184500-mofka-pmdk-volume-afterloop/002-mofka-append_throughput-default-pmdk-after_loop-after_loop-n4-c4-s16384-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260516-155000-mofka-pmdk-64k-afterloop-group-timeout/001-mofka-append_throughput-default-pmdk-after_loop-after_loop-n4-c4-s65536-o10000-t1/mofka/metrics.json",
                        },
                    ),
                ],
            },
            "append_then_catchup": {
                "sizes": [1024, 65536],
                "cells": [
                    accepted(
                        "chronolog_live_tail_catchup",
                        notes=(
                            "ChronoLog append followed by live ReplayStory/tail catch-up after writers finish. "
                            "This is memory/live-path evidence with no durable/storage completion proof. Writers "
                            "release the shared story after the reader completes so the live tail remains available "
                            "for the measured catch-up window."
                        ),
                        paths={
                            1024: ".agent/results/20260517-104500-chronolog-live-catchup-full-afterreader-1k/chronolog/metrics.json",
                            65536: ".agent/results/20260517-110000-chronolog-live-catchup-full-afterreader-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_live_tail_catchup_scaling_2n",
                        notes=(
                            "2-node scaling coverage for ChronoLog append followed by live ReplayStory/tail "
                            "catch-up after writers finish. This is memory/live-path evidence with no durable "
                            "storage completion proof. Writers release the shared story after the reader completes."
                        ),
                        paths={
                            1024: ".agent/results/20260518-083000-node-scaling-2n-chronolog-live-catchup-1k/chronolog/metrics.json",
                            65536: ".agent/results/20260518-084000-node-scaling-2n-chronolog-live-catchup-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_keeper_cursor_catchup",
                        notes="ChronoLog append followed by Keeper-cursor tail catch-up; Keeper journal durability, not archive storage.",
                        paths={
                            1024: ".agent/results/20260517-114500-mixed-c4-1k-afterreader-fix/chronolog/metrics.json",
                            65536: ".agent/results/20260515-171000-mixed-tail-batched-writer-keeper-cursor-c4-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_journal_tail_catchup",
                        notes=(
                            "ChronoLog append ACK occurs after Keeper-local journal group fdatasync and before "
                            "tail-index publication, then a reader performs Keeper-local journal tail catch-up "
                            "after writers finish. This proves post-ACK catch-up/readability from the Keeper "
                            "journal tail, not immediate live-tail visibility at ACK time and not archive/storage "
                            "range retrieval. Throughput uses tail_retrieval_throughput_ops_per_sec so this cell "
                            "is a read-path catch-up comparison, not append workflow throughput."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            1024: ".agent/results/20260517-141500-wal-ack-journal-tail-catchup-1k/chronolog/metrics.json",
                            65536: ".agent/results/20260517-142000-wal-ack-journal-tail-catchup-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_bounded_tail_batch_64MiB",
                        notes=(
                            "ChronoLog append ACK occurs after Keeper-local journal group fdatasync and before "
                            "tail-index publication, then a reader performs bounded Keeper-cursor journal-tail "
                            "catch-up after writers finish. This cell uses a 64MiB per-Keeper cursor RPC payload "
                            "cap and is a streaming/cursor tail semantic. It must not be merged with full ReplayStory "
                            "or unbounded keeper_cursor rows. Throughput uses tail_retrieval_throughput_ops_per_sec."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260517-183000-chunked-tail-cursor-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_bounded_tail_batch_32MiB",
                        notes=(
                            "ChronoLog append ACK occurs after Keeper-local journal group fdatasync and before "
                            "tail-index publication, then a reader performs bounded Keeper-cursor journal-tail "
                            "catch-up after writers finish. This cell uses a 32MiB per-Keeper cursor RPC payload "
                            "cap and is a streaming/cursor tail semantic. It must not be merged with full ReplayStory, "
                            "unbounded keeper_cursor, or 64MiB bounded cursor rows. Throughput uses "
                            "tail_retrieval_throughput_ops_per_sec."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260517-161000-chunked-tail-cursor-32m-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_bounded_tail_batch_16MiB",
                        notes=(
                            "ChronoLog append ACK occurs after Keeper-local journal group fdatasync and before "
                            "tail-index publication, then a reader performs bounded Keeper-cursor journal-tail "
                            "catch-up after writers finish. This cell uses a 16MiB per-Keeper cursor RPC payload "
                            "cap and is a streaming/cursor tail semantic. It must not be merged with full ReplayStory, "
                            "unbounded keeper_cursor, 32MiB bounded cursor, or 64MiB bounded cursor rows. Throughput "
                            "uses tail_retrieval_throughput_ops_per_sec."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260517-163000-chunked-tail-cursor-16m-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_packed_api_bounded_tail_batch_16MiB",
                        notes=(
                            "ChronoLog append ACK occurs after Keeper-local journal group fdatasync and before "
                            "tail-index publication, then a reader performs bounded Keeper-cursor journal-tail "
                            "catch-up through the public PackedReplayBatch API. The API returns sorted metadata "
                            "plus payload blobs and avoids eager Event::logRecord reconstruction; payload strings "
                            "are materialized only on demand. This is a streaming/cursor tail semantic with a "
                            "16MiB per-Keeper cursor RPC payload cap, not full ReplayStory, not metadata-only "
                            "diagnostic output, and not archive/storage range retrieval. Throughput uses "
                            "tail_retrieval_throughput_ops_per_sec."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260517-172800-packed-api-16m-64k/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_packed_api_bulk_bounded_tail_batch_16MiB",
                        notes=(
                            "ChronoLog append ACK occurs after Keeper-local journal group fdatasync and before "
                            "tail-index publication, then a reader performs bounded Keeper-cursor journal-tail "
                            "catch-up through the public PackedReplayBatch API. Metadata returns in the RPC "
                            "response while packed payload bytes transfer through a client-supplied Thallium "
                            "bulk buffer. This is a default-off 16MiB per-Keeper cursor RPC payload cap "
                            "semantic, not full ReplayStory, not metadata-only diagnostic output, and not "
                            "archive/storage range retrieval. Throughput uses tail_retrieval_throughput_ops_per_sec."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-104500-4n-journal-catchup-64k-packed-bulk/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_packed_api_bulk_bounded_tail_batch_16MiB_scaling_2n",
                        notes=(
                            "2-node scaling coverage for the durable Keeper-local journal catch-up semantic: "
                            "append ACK occurs after Keeper-local journal group fdatasync and before tail-index "
                            "publication, then a reader performs bounded Keeper-cursor journal-tail catch-up "
                            "through the public PackedReplayBatch API with Thallium bulk payload transfer. This "
                            "row uses node-local /mnt/nvme Keeper journals, a 16MiB per-Keeper cursor payload cap, "
                            "and tail_retrieval_throughput_ops_per_sec as the measurement field. It is not the "
                            "live ReplayStory memory-tail semantic and should not be merged with live-return rows."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-101500-2n-journal-catchup-64k-packed-bulk-tauinstall/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_packed_api_bulk_vectored_bounded_tail_batch_16MiB",
                        notes=(
                            "ChronoLog append ACK occurs after Keeper-local journal group fdatasync and before "
                            "tail-index publication, then a reader performs bounded Keeper-cursor journal-tail "
                            "catch-up through the public PackedReplayBatch API. Metadata returns in the RPC "
                            "response while packed payload bytes transfer through a client-supplied Thallium "
                            "bulk buffer. This explicit read-mode cell uses vectored Keeper journal payload "
                            "reads under the same 16MiB per-Keeper cursor payload cap. It is a separate "
                            "semantic/knob row from the default auto/direct packed-bulk cell; 1KiB guardrail "
                            "improved tail catch-up but was neutral/slightly lower for full workflow throughput, "
                            "so this is not a global default promotion. The 64KiB row includes the accepted "
                            "read-plan cleanup that removed a redundant physical-order sort in the packed "
                            "vectored tail path. Throughput uses tail_retrieval_throughput_ops_per_sec."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            1024: ".agent/results/20260517-194500-packed-bulk-vectored-1k-guardrail/chronolog/metrics.json",
                            65536: ".agent/results/20260517-210330/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_packed_api_bulk_auto_vectored_bounded_tail_batch_16MiB",
                        notes=(
                            "ChronoLog append ACK occurs after Keeper-local journal group fdatasync and before "
                            "tail-index publication, then a reader performs bounded Keeper-cursor journal-tail "
                            "catch-up through the public PackedReplayBatch API. Metadata returns in the RPC "
                            "response while packed payload bytes transfer through a client-supplied Thallium "
                            "bulk buffer. This post-change auto-mode cell uses vectored Keeper journal payload "
                            "reads only when the auto policy sees multi-event large-payload tail batches; the "
                            "1KiB guardrail remains direct-read in auto mode. It is the accepted source-policy "
                            "row after the 2026-05-18 auto-vectored Keeper tail-read change, separate from older "
                            "forced-vectored knob experiments and older auto/direct baselines. Throughput uses "
                            "tail_retrieval_throughput_ops_per_sec."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            1024: ".agent/results/20260518-131500-4n-durable-packed-auto-vectored-code-1k-guardrail/chronolog/metrics.json",
                            65536: ".agent/results/20260518-130000-4n-durable-packed-auto-vectored-code/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_wal_ack_before_tail_publish_keeper_cursor_packed_api_bulk_auto_vectored_bounded_tail_batch_16MiB_scaling_8n",
                        notes=(
                            "8-node scaling coverage for the durable Keeper-local journal packed-bulk catch-up "
                            "semantic after the auto-vectored Keeper tail-read policy. Append ACK occurs after "
                            "Keeper-local journal group fdatasync and before tail-index publication, then a reader "
                            "performs bounded Keeper-cursor journal-tail catch-up through the public PackedReplayBatch "
                            "API with Thallium bulk payload transfer. This row uses node-local /mnt/nvme Keeper "
                            "journals, a 16MiB per-Keeper cursor payload cap, and tail_retrieval_throughput_ops_per_sec "
                            "as the measurement field. It is live/tail catch-up evidence, not archive/storage range "
                            "retrieval and not a Kafka/Mofka storage-backed comparison."
                        ),
                        throughput_field="tail_retrieval_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-040500-8n-journal-catchup-64k-packed-bulk/001-chronolog-mixed_append_tail-n8-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_consumer_catchup",
                        notes="Kafka fixed baseline append plus consumer catch-up with acks=0.",
                        paths={
                            1024: ".agent/results/20260515-174500-kafka-range-c4-1k-acks0/001-kafka-range_retrieval-n4-c4-s1024-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260515-174500-kafka-range-c4-64k-acks0/001-kafka-range_retrieval-n4-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_consumer_catchup_scaling_2n",
                        notes="2-node Kafka fixed baseline append plus consumer catch-up with acks=0.",
                        paths={
                            1024: ".agent/results/20260518-032000-node-scaling-2n-kafka-range-1k/001-kafka-range_retrieval-n2-c4-s1024-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-042000-node-scaling-2n-kafka-range-64k/001-kafka-range_retrieval-n2-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_consumer_catchup_scaling_8n",
                        notes=(
                            "8-node Kafka fixed baseline append plus consumer catch-up with acks=0. "
                            "This 64KiB row uses 4 clients, 2500 messages per client, 10000 total "
                            "messages, and 655360000 total payload bytes. No broker acknowledgement "
                            "durability boundary; do not compare as storage-backed durability."
                        ),
                        paths={
                            65536: ".agent/results/20260518-174500-8n-range-64k/002-kafka-range_retrieval-n8-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_consumer_catchup_rf1",
                        notes="Kafka fixed baseline append plus consumer catch-up with acks=all and replication factor 1.",
                        paths={
                            1024: ".agent/results/20260515-161500-kafka-range-c4-1k-acksall/kafka/metrics.json",
                            65536: ".agent/results/20260515-161500-kafka-range-c4-64k-acksall/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_consumer_catchup_rf1_scaling_2n",
                        notes="2-node Kafka fixed baseline append plus consumer catch-up with acks=all and replication factor 1.",
                        paths={
                            1024: ".agent/results/20260518-032000-node-scaling-2n-kafka-range-1k/002-kafka-range_retrieval-n2-c4-s1024-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-042000-node-scaling-2n-kafka-range-64k/002-kafka-range_retrieval-n2-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_consumer_catchup_rf1_scaling_8n",
                        notes=(
                            "8-node Kafka fixed baseline append plus consumer catch-up with acks=all and "
                            "replication factor 1. This 64KiB row uses 4 clients, 2500 messages per "
                            "client, 10000 total messages, and 655360000 total payload bytes. Treat as "
                            "leader/in-sync-replica acknowledgement for this RF1 harness, not as "
                            "multi-replica durability."
                        ),
                        paths={
                            65536: ".agent/results/20260518-174500-8n-range-64k/003-kafka-range_retrieval-n8-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_afterloop_noflush_pull_catchup",
                        notes="Mofka memory partition, after-loop pull/catch-up semantics, no explicit producer flush.",
                        paths={
                            1024: ".agent/results/20260515-162000-mofka-memory-range-c4-1k-afterloop-noflush/mofka/metrics.json",
                            65536: ".agent/results/20260515-162500-mofka-memory-range-c4-64k-afterloop-noflush-retry/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_afterloop_noflush_pull_catchup_scaling_2n",
                        notes="2-node Mofka memory partition, after-loop pull/catch-up semantics, no explicit producer flush.",
                        paths={
                            1024: ".agent/results/20260518-033000-node-scaling-2n-mofka-memory-range-1k/001-mofka-range_retrieval-memory-memory-after_loop-no_flush-n2-c4-s1024-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-043000-node-scaling-2n-mofka-memory-range-64k/001-mofka-range_retrieval-memory-memory-after_loop-no_flush-n2-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_afterloop_noflush_pull_catchup_scaling_8n",
                        notes=(
                            "8-node Mofka memory partition, after-loop producer wait, no explicit flush, "
                            "then consumer pull/catch-up. This 64KiB row uses 4 clients, 2500 messages "
                            "per client, 10000 total messages, and 655360000 total payload bytes. "
                            "Memory/live evidence only, not durable storage evidence."
                        ),
                        paths={
                            65536: ".agent/results/20260518-174500-8n-range-64k/004b-mofka-range_retrieval-memory-memory-after_loop-none-n8-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_backed_catchup",
                        notes="Mofka default partition with precreated PMDK Warabi storage target, per-event producer wait and flush-after-loop, then consumer pull catch-up.",
                        paths={
                            1024: ".agent/results/20260515-233000-mofka-pmdk-c4-1k-range-waitflush/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n4-c4-s1024-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260515-233000-mofka-pmdk-c4-64k-range-waitflush-1gpool/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n4-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_backed_catchup_scaling_2n",
                        notes="2-node Mofka default partition with precreated PMDK Warabi storage target, per-event producer wait and flush-after-loop, then consumer pull catch-up.",
                        paths={
                            1024: ".agent/results/20260518-034000-node-scaling-2n-mofka-pmdk-range-1k/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n2-c4-s1024-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-044000-node-scaling-2n-mofka-pmdk-range-64k/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n2-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_backed_catchup_scaling_8n",
                        notes=(
                            "8-node Mofka default partition with precreated 1GiB PMDK Warabi storage "
                            "targets, per-event producer wait, flush-after-loop, then consumer pull/catch-up. "
                            "This 64KiB row uses 4 clients, 2500 messages per client, 10000 total messages, "
                            "and 655360000 total payload bytes. Storage-backed catch-up evidence, but not "
                            "archive subrange retrieval."
                        ),
                        paths={
                            65536: ".agent/results/20260518-174500-8n-range-64k/005-mofka-range_retrieval-default-pmdk-per_event-after_loop-n8-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                ],
            },
            "archive_storage_range": {
                "sizes": [1024, 65536],
                "notes": "Use ChronoLog workflow throughput for end-to-end archive/storage comparisons. Use range-readback throughput only for read-path analysis.",
                "cells": [
                    accepted(
                        "chronolog_archive_storage_range",
                        notes=(
                            "ChronoLog waits for archive event-count evidence and validates ChronoPlayer HDF5 "
                            "subrange readback. Current rows use drain-gated archive completion with parallel "
                            "Keeper stop, Keeper drain Margo progress thread=1, Keeper fast-wire, and Grapher "
                            "retire-on-stop enabled. The archive writer uses the post-split raw_blob payload/HDF5 "
                            "serialization path and the post-split Grapher extraction default of 2."
                        ),
                        throughput_field="workflow_total_message_throughput_ops_per_sec",
                        paths={
                            1024: ".agent/results/20260516-120000-keeper-drain-progress-thread-default1k/001-chronolog-archive_range_retrieval-n4-c4-s1024-o10000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260516-115000-keeper-drain-progress-thread-default64k/001-chronolog-archive_range_retrieval-n4-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_2n",
                        notes=(
                            "2-node scaling coverage for ChronoLog archive/storage range retrieval. The runner "
                            "waits for archive event-count evidence and validates ChronoPlayer HDF5 subrange "
                            "readback. This is storage/archive evidence, not live Keeper tail evidence."
                        ),
                        throughput_field="workflow_total_message_throughput_ops_per_sec",
                        paths={
                            1024: ".agent/results/20260518-031000-node-scaling-2n-chronolog-archive-range-1k/001-chronolog-archive_range_retrieval-n2-c4-s1024-o10000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260518-041000-node-scaling-2n-chronolog-archive-range-64k/001-chronolog-archive_range_retrieval-n2-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_8n",
                        notes=(
                            "8-node scaling coverage for ChronoLog archive/storage range retrieval. Rows use "
                            "4 clients with size-specific message counts and validate ChronoPlayer HDF5 subrange "
                            "readback after archive event-count evidence. This is storage/archive evidence, not "
                            "live Keeper tail evidence. The 64KiB row splits archive-file publication timing "
                            "from validator-side HDF5 event-count/readback cost."
                        ),
                        throughput_field="workflow_total_message_throughput_ops_per_sec",
                        paths={
                            1024: ".agent/results/20260518-044800-8n-range-1k/001-chronolog-archive_range_retrieval-n8-c4-s1024-o10000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260518-073000-8n-range-64k-publication-split/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_8n_parallel_readback",
                        notes=(
                            "8-node 64KiB ChronoLog archive/storage range retrieval counterfactual with "
                            "parallel_thread archive readback issue mode. Accepted as a separate readback-path "
                            "attribution row, not a replacement for the inline archive/storage baseline, because "
                            "the readback-only measurement improved while full workflow throughput did not."
                        ),
                        throughput_field="workflow_total_message_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-085000-8n-range-64k-parallel-readback/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_8n_parallel_readback_async_close",
                        notes=(
                            "8-node 64KiB ChronoLog archive/storage range retrieval with parallel_thread readback "
                            "and raw_blob async-close archive writer overlap enabled. This keeps archive publication "
                            "semantics intact by waiting for payload close before final rename, but overlaps the "
                            "payload close with HDF5 metadata work. Accepted as a separate optimized semantic row "
                            "after one smoke and two non-smoke runs passed exact archive/readback gates."
                        ),
                        throughput_field="workflow_total_message_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-095000-8n-range-64k-raw-blob-async-close-repeat/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_8n_archive_manifest",
                        notes=(
                            "8-node 64KiB ChronoLog archive/storage range retrieval with raw_blob async-close "
                            "and archive publication manifests with event-time indexes. ChronoLog writes a small "
                            "manifest sidecar after archive files are closed and published; the harness uses it for "
                            "event-count confirmation and exact range selection before falling back to HDF5 reads. "
                            "Accepted as an archive/storage semantic row because ChronoPlayer HDF5/raw_blob range "
                            "readback is still validated."
                        ),
                        throughput_field="workflow_total_message_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-130500-8n-range-64k-manifest-event-times/001-chronolog-archive_range_retrieval-n8-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_8n_parallel_keeper_stop",
                        notes=(
                            "8-node 64KiB ChronoLog archive/storage range retrieval with raw_blob async-close, "
                            "archive event-time manifests, and Visor parallel Keeper-stop notification enabled. "
                            "Release-path profiling showed the prior row was dominated by sequential Keeper stop "
                            "RPCs at ReleaseStory; this opt-in semantic keeps archive/readback validation intact "
                            "while reducing the release/control-path boundary."
                        ),
                        throughput_field="workflow_total_message_active_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-090012-8n-range-64k-parallel-keeper-stop/001-chronolog-archive_range_retrieval-n8-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_8n_grapher_receive_x4",
                        notes=(
                            "8-node 64KiB ChronoLog archive/storage range retrieval with parallel Keeper-stop "
                            "and Grapher recording receive concurrency increased to four xstreams and four handlers. "
                            "Accepted as a release/drain counterfactual because it reduced Keeper StopStory drain "
                            "and Grapher receive outliers while preserving archive/readback validation. It is not "
                            "promoted over the parallel Keeper-stop throughput baseline because archive publication "
                            "variance was slower in the x4 rows."
                        ),
                        throughput_field="workflow_total_message_active_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-8n-range-64k-grapher-receive-x4-repeat/001-chronolog-archive_range_retrieval-n8-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_8n_async_publish_threads4",
                        notes=(
                            "8-node 64KiB ChronoLog archive/storage range retrieval with raw_blob async-close, "
                            "archive event-time manifests, parallel Keeper-stop, and default-off four-worker "
                            "async archive publication. This row keeps archive/readback validation intact while "
                            "moving raw payload close, final archive rename, and manifest publication to a "
                            "publisher pool. Accepted as an improvement candidate after the single-publisher "
                            "async-publish row was rejected for serializing close waits."
                        ),
                        throughput_field="workflow_total_message_active_throughput_ops_per_sec",
                        paths={
                            1024: ".agent/results/20260518-8n-range-1k-async-publish-threads4-guardrail/001-chronolog-archive_range_retrieval-n8-c4-s1024-o10000-t1/chronolog/metrics.json",
                            65536: ".agent/results/20260518-8n-range-64k-async-publish-threads4/001-chronolog-archive_range_retrieval-n8-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_storage_range_scaling_8n_async_publish_threads4_parallel_keeper_stop_current",
                        notes=(
                            "Current-build 8-node ChronoLog archive/storage range retrieval with raw_blob "
                            "async-close, close-before-publication four-worker async archive publish, and "
                            "parallel Keeper-stop enabled. This keeps the stronger archive/storage semantic "
                            "while revalidating the accepted release-path knob after later archive-writer "
                            "counterfactuals. The 1KiB row is the matched small-message guardrail."
                        ),
                        throughput_field="workflow_total_message_active_throughput_ops_per_sec",
                        paths={
                            1024: ".agent/results/20260518-174938-8n-range-1k-async-publish-x4-parallel-keeper-stop-guardrail/chronolog/metrics.json",
                            65536: ".agent/results/20260518-174510-8n-range-64k-async-publish-x4-parallel-keeper-stop/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_ready_stop_boundary_2n",
                        notes=(
                            "2-node 64KiB default-off ChronoLog archive-ready stop semantic. Grapher "
                            "stop_story_recording waits for story-specific archive extraction drain before "
                            "responding, so archive readiness is represented at the service boundary instead "
                            "of only by harness-side file polling. Accepted as semantic evidence, not a "
                            "throughput optimization claim."
                        ),
                        throughput_field="workflow_total_message_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-073000-grapher-archive-drain-64k/001-chronolog-archive_range_retrieval-n2-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "chronolog_archive_ready_stop_boundary_8n",
                        notes=(
                            "8-node 64KiB default-off ChronoLog archive-ready stop semantic. Grapher "
                            "stop_story_recording waits for story-specific archive extraction drain before "
                            "responding, making archive readiness visible at the service boundary rather than "
                            "only through harness-side file polling. This row is accepted as stricter "
                            "storage-boundary semantic evidence and rejected as the default throughput path "
                            "because release now pays the archive-drain wait."
                        ),
                        throughput_field="workflow_total_message_active_throughput_ops_per_sec",
                        paths={
                            65536: ".agent/results/20260518-110500-8n-range-64k-grapher-archive-drain/001-chronolog-archive_range_retrieval-n8-c4-s65536-o2500-t1/chronolog/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_log_consumer_catchup_caveat",
                        notes="Semantic caveat: Kafka log consumer catch-up is not archive subrange retrieval.",
                        paths={
                            1024: ".agent/results/20260515-174500-kafka-range-c4-1k-acks0/001-kafka-range_retrieval-n4-c4-s1024-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260515-174500-kafka-range-c4-64k-acks0/001-kafka-range_retrieval-n4-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_log_consumer_catchup_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Kafka acks=0 append followed by consumer catch-up. "
                            "Semantic caveat: this is not archive subrange retrieval and has no broker "
                            "acknowledgement durability boundary."
                        ),
                        paths={
                            1024: ".agent/results/20260518-032000-node-scaling-2n-kafka-range-1k/001-kafka-range_retrieval-n2-c4-s1024-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-042000-node-scaling-2n-kafka-range-64k/001-kafka-range_retrieval-n2-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_0_log_consumer_catchup_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Kafka acks=0 append followed by consumer catch-up. "
                            "Semantic caveat: this is not archive subrange retrieval and has no broker "
                            "acknowledgement durability boundary."
                        ),
                        paths={
                            1024: ".agent/results/20260518-044800-8n-range-1k/002-kafka-range_retrieval-n8-c4-s1024-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-174500-8n-range-64k/002-kafka-range_retrieval-n8-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_log_consumer_catchup_caveat",
                        notes="Semantic caveat: Kafka log consumer catch-up is not archive subrange retrieval.",
                        paths={
                            1024: ".agent/results/20260515-161500-kafka-range-c4-1k-acksall/kafka/metrics.json",
                            65536: ".agent/results/20260515-161500-kafka-range-c4-64k-acksall/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_log_consumer_catchup_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Kafka acks=all RF1 append followed by consumer catch-up. "
                            "Semantic caveat: this is not archive subrange retrieval."
                        ),
                        paths={
                            1024: ".agent/results/20260518-032000-node-scaling-2n-kafka-range-1k/002-kafka-range_retrieval-n2-c4-s1024-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-042000-node-scaling-2n-kafka-range-64k/002-kafka-range_retrieval-n2-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "kafka_acks_all_log_consumer_catchup_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Kafka acks=all RF1 append followed by consumer catch-up. "
                            "Semantic caveat: this is not archive subrange retrieval and not multi-replica durability."
                        ),
                        paths={
                            1024: ".agent/results/20260518-044800-8n-range-1k/003-kafka-range_retrieval-n8-c4-s1024-o10000-t1/kafka/metrics.json",
                            65536: ".agent/results/20260518-174500-8n-range-64k/003-kafka-range_retrieval-n8-c4-s65536-o2500-t1/kafka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_pull_catchup_caveat",
                        notes="Semantic caveat: Mofka memory pull/catch-up is not storage-backed archive subrange retrieval.",
                        paths={
                            1024: ".agent/results/20260515-162000-mofka-memory-range-c4-1k-afterloop-noflush/mofka/metrics.json",
                            65536: ".agent/results/20260515-162500-mofka-memory-range-c4-64k-afterloop-noflush-retry/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_pull_catchup_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Mofka memory partition, after-loop producer wait, "
                            "no explicit flush, then consumer pull/catch-up. Memory/live evidence only; not "
                            "durable storage and not archive subrange retrieval."
                        ),
                        paths={
                            1024: ".agent/results/20260518-033000-node-scaling-2n-mofka-memory-range-1k/001-mofka-range_retrieval-memory-memory-after_loop-no_flush-n2-c4-s1024-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-043000-node-scaling-2n-mofka-memory-range-64k/001-mofka-range_retrieval-memory-memory-after_loop-no_flush-n2-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_memory_pull_catchup_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Mofka memory partition, after-loop producer wait, "
                            "no explicit flush, then consumer pull/catch-up. Memory/live evidence only; not "
                            "durable storage and not archive subrange retrieval."
                        ),
                        paths={
                            65536: ".agent/results/20260518-174500-8n-range-64k/004b-mofka-range_retrieval-memory-memory-after_loop-none-n8-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_backed_range",
                        notes="Semantic caveat: Mofka PMDK consumer pull/catch-up is storage-backed but is not archive subrange retrieval.",
                        paths={
                            1024: ".agent/results/20260515-233000-mofka-pmdk-c4-1k-range-waitflush/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n4-c4-s1024-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260515-233000-mofka-pmdk-c4-64k-range-waitflush-1gpool/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n4-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_backed_range_scaling_2n",
                        notes=(
                            "2-node scaling coverage for Mofka default partition with PMDK storage target, "
                            "per-event wait, flush-after-loop, then consumer pull/catch-up. Storage-backed "
                            "catch-up evidence, but not archive subrange retrieval."
                        ),
                        paths={
                            1024: ".agent/results/20260518-034000-node-scaling-2n-mofka-pmdk-range-1k/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n2-c4-s1024-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-044000-node-scaling-2n-mofka-pmdk-range-64k/001-mofka-range_retrieval-default-pmdk-per_event-after_loop-n2-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                    accepted(
                        "mofka_storage_backed_range_scaling_8n",
                        notes=(
                            "8-node scaling coverage for Mofka default partition with PMDK storage targets, "
                            "per-event wait, flush-after-loop, then consumer pull/catch-up. The 1KiB row uses "
                            "precreated 512MiB targets with longer Flock startup ping settings; the 64KiB row "
                            "uses precreated 1GiB targets. Storage-backed catch-up evidence, but not archive "
                            "subrange retrieval."
                        ),
                        paths={
                            1024: ".agent/results/20260518-044800-8n-range-1k/005b-mofka-range_retrieval-default-pmdk-per_event-after_loop-n8-c4-s1024-o10000-t1/mofka/metrics.json",
                            65536: ".agent/results/20260518-174500-8n-range-64k/005-mofka-range_retrieval-default-pmdk-per_event-after_loop-n8-c4-s65536-o2500-t1/mofka/metrics.json",
                        },
                    ),
                ],
            },
        },
    }


def write_summary(manifest: dict[str, Any], output_dir: Path) -> None:
    lines = [
        "# Six-Way Matrix Manifest Refresh",
        "",
        f"Timestamp: {manifest['timestamp']}",
        "",
        "Status: complete",
        "",
        "This refresh is generated from source metrics and keeps the required workload counts in every accepted row.",
        "For ChronoLog `archive_storage_range`, the comparison throughput is `workflow_total_message_throughput_ops_per_sec`; the readback-only value remains available as `range_readback_throughput_ops_per_sec`.",
        "",
        "| Workload | Cell | Size | Status | Throughput | Throughput Field | Messages/Client | Total Messages | Payload Bytes | Clients | Nodes |",
        "|---|---|---:|---|---:|---|---:|---:|---:|---:|---:|",
    ]
    for workload, workload_data in manifest["workloads"].items():
        for cell in workload_data["cells"]:
            if cell["status"] != "accepted":
                lines.append(f"| {workload} | {cell['cell']} | all | {cell['status']} |  |  |  |  |  |  |  |")
                continue
            for size, metrics in cell["rows"].items():
                lines.append(
                    "| {workload} | {cell} | {size} | accepted | {thr:.3f} | {field} | {per_client} | {total} | {payload} | {clients} | {nodes} |".format(
                        workload=workload,
                        cell=cell["cell"],
                        size=size,
                        thr=metrics["throughput_ops_per_sec"],
                        field=metrics["measurement_throughput_field"],
                        per_client=metrics["message_count_per_client"],
                        total=metrics["total_message_count"],
                        payload=metrics["total_payload_bytes"],
                        clients=metrics["parallel_client_count"],
                        nodes=metrics["node_count"],
                    )
                )
    lines.extend(
        [
            "",
            "Decision:",
            "",
            "- Memory/live, Keeper-journal durable, and archive/storage rows remain separate semantic classes.",
            "- ChronoLog archive/storage rows must not be compared using `range_readback_throughput_ops_per_sec` as if it were append/archive workflow throughput.",
            "- Mofka PMDK/default storage append and consumer catch-up cells are accepted for the default-partition workflows; Mofka consumer pull/catch-up is still a different read semantic than ChronoLog archive subrange retrieval.",
        ]
    )
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    output_dir = REPO_ROOT / args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest = build_manifest()
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    write_summary(manifest, output_dir)
    print(rel(output_dir / "manifest.json"))
    print(rel(output_dir / "summary.md"))


if __name__ == "__main__":
    main()
