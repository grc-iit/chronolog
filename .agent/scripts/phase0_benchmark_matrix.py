#!/usr/bin/env python3
"""Run a tunable Phase 0 benchmark matrix and collect common metrics."""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import os
import random
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
RESULTS_ROOT = REPO_ROOT / ".agent" / "results"
if str(REPO_ROOT / ".agent" / "scripts") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / ".agent" / "scripts"))

from chronolog_append_durable import control_path_profile_metrics


SYSTEM_SCRIPTS = {
    "chronolog": REPO_ROOT / ".agent" / "scripts" / "chronolog_run_append_distributed.sh",
    "kafka": REPO_ROOT / ".agent" / "scripts" / "kafka_run_append_distributed.sh",
    "mofka": REPO_ROOT / ".agent" / "scripts" / "mofka_run_append_distributed.sh",
}


WORKFLOW_BY_SYSTEM = {
    "append_throughput": {"chronolog", "kafka", "mofka"},
    "append_latency": {"chronolog", "kafka", "mofka"},
    "append_durable": {"chronolog"},
    "archive_range_retrieval": {"chronolog"},
    "range_retrieval": {"chronolog", "kafka", "mofka"},
    "mixed_append_tail": {"chronolog"},
    "keeper_restart_recovery": {"chronolog"},
}

CHRONOLOG_MULTI_CLIENT_WORKFLOWS = {
    "append_throughput",
    "mixed_append_tail",
    "keeper_restart_recovery",
    "archive_range_retrieval",
}

ARCHIVE_COMPLETION_MODES = {"archive_readback"}
APPEND_THROUGHPUT_COMPLETION_MODES = {
    "",
    "live_return",
    "keeper_journal_fdatasync",
    "keeper_journal_fdatasync_tail_only",
    "keeper_journal_group_fdatasync",
    "keeper_journal_group_fdatasync_early_ack",
    "keeper_journal_group_fdatasync_async_drain",
    "keeper_journal_group_fdatasync_wal_drain",
    "keeper_journal_group_fdatasync_tail_only",
    "keeper_journal_group_commit_tail_only",
    "keeper_journal_group_commit_deferred_tail_only",
}


def timestamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def csv_ints(value: str) -> list[int]:
    return [int(part) for part in value.split(",") if part.strip()]


def csv_strings(value: str) -> list[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def parse_chronolog_client_execution_modes(value: str) -> list[str]:
    modes = []
    for mode in csv_strings(value):
        normalized = "threads" if mode == "threaded" else mode
        if normalized not in {"sequential", "threads", "parallel"}:
            raise SystemExit(
                f"unsupported ChronoLog client execution mode {mode!r}; "
                "expected sequential, threads, threaded, or parallel"
            )
        modes.append(normalized)
    return modes


def chronolog_producer_wait_label(wait_policy: str, outstanding: int | str, batch_size: int | str) -> str:
    outstanding_int = int(outstanding or 1)
    batch_size_int = int(batch_size or 1)
    effective_policy = wait_policy or "auto"
    if effective_policy == "auto":
        effective_policy = "per_event" if outstanding_int <= 1 else "bounded_outstanding"
    if effective_policy == "after_loop":
        return (
            "after_loop_wait"
            if batch_size_int <= 1
            else f"after_loop_wait_batch_{batch_size_int}_records"
        )
    if effective_policy == "per_event":
        return "per_event_wait" if batch_size_int <= 1 else f"batch_wait_after_{batch_size_int}_records"
    if effective_policy == "bounded_outstanding":
        return f"bounded_outstanding_wait_after_{outstanding_int}_calls_batch_{batch_size_int}"
    if effective_policy == "bounded_futures":
        return f"bounded_futures_wait_after_{outstanding_int}_keeper_futures_batch_{batch_size_int}"
    if effective_policy == "bounded_api":
        return f"bounded_api_wait_after_{outstanding_int}_calls_batch_{batch_size_int}"
    if effective_policy == "bounded_per_keeper":
        return f"bounded_per_keeper_wait_after_{outstanding_int}_keeper_futures_batch_{batch_size_int}"
    raise SystemExit(
        f"unsupported ChronoLog producer wait policy {wait_policy!r}; "
        "expected auto, per_event, bounded_outstanding, bounded_futures, bounded_api, bounded_per_keeper, or after_loop"
    )


def chronolog_completion_mode_is_valid_for_workflow(workflow: str, completion_mode: str) -> bool:
    """Prevent the matrix from producing rows with misleading ChronoLog semantics."""
    if workflow == "append_throughput":
        return completion_mode in APPEND_THROUGHPUT_COMPLETION_MODES
    if workflow == "append_durable":
        return completion_mode in ARCHIVE_COMPLETION_MODES
    if workflow == "archive_range_retrieval":
        return completion_mode in ARCHIVE_COMPLETION_MODES
    if workflow == "keeper_restart_recovery":
        return completion_mode not in ARCHIVE_COMPLETION_MODES
    if workflow == "mixed_append_tail":
        return completion_mode not in ARCHIVE_COMPLETION_MODES
    return completion_mode in {"", "live_return"}


def chronolog_client_count_supported(workflow: str, client_count: int) -> bool:
    return client_count == 1 or workflow in CHRONOLOG_MULTI_CLIENT_WORKFLOWS


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--systems", default="chronolog,kafka,mofka")
    parser.add_argument("--workflows", default="append_throughput")
    parser.add_argument("--node-counts", default="2")
    parser.add_argument("--message-sizes", default="1024")
    parser.add_argument("--operation-counts", default="1000")
    parser.add_argument("--client-counts", default="1")
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--shuffle-runs", action="store_true", help="Shuffle expanded runs before execution.")
    parser.add_argument("--shuffle-seed", type=int, default=0, help="Seed used with --shuffle-runs.")
    parser.add_argument("--partition", default=os.environ.get("CHRONOLOG_SLURM_PARTITION", "debug"))
    parser.add_argument("--nodelist", default=os.environ.get("CHRONOLOG_NODELIST", ""))
    parser.add_argument("--slurm-time", default="00:10:00")
    parser.add_argument("--chronolog-install-dir", default=str(REPO_ROOT / ".agent" / "install-tau" / "chronolog"))
    parser.add_argument("--chronolog-profile-mode", default="none")
    parser.add_argument("--chronolog-startup-sleep", type=int, default=10)
    parser.add_argument("--chronolog-completion-modes", default="live_return")
    parser.add_argument("--chronolog-data-collection-poll-intervals-us", default="1000000")
    parser.add_argument("--chronolog-archive-event-count-poll-intervals-seconds", default="1.0")
    parser.add_argument("--chronolog-archive-readback-modes", default="inline")
    parser.add_argument(
        "--chronolog-keeper-data-collection-streams",
        default=os.environ.get("CHRONOLOG_KEEPER_DATA_COLLECTION_STREAMS", "3"),
    )
    parser.add_argument(
        "--chronolog-keeper-data-collection-threads-per-stream",
        default=os.environ.get("CHRONOLOG_KEEPER_DATA_COLLECTION_THREADS_PER_STREAM", "2"),
    )
    parser.add_argument(
        "--chronolog-grapher-data-collection-streams",
        default=os.environ.get("CHRONOLOG_GRAPHER_DATA_COLLECTION_STREAMS", "3"),
    )
    parser.add_argument(
        "--chronolog-grapher-data-collection-threads-per-stream",
        default=os.environ.get("CHRONOLOG_GRAPHER_DATA_COLLECTION_THREADS_PER_STREAM", "2"),
    )
    parser.add_argument(
        "--chronolog-grapher-inactive-story-delay-seconds",
        default=os.environ.get("CHRONOLOG_GRAPHER_INACTIVE_STORY_DELAY_SECONDS", ""),
        help=(
            "Comma-separated Grapher pipeline retirement delays. Empty means runner default: "
            "append_durable archive_readback uses 3 seconds explicitly, other workflows use installed config."
        ),
    )
    parser.add_argument(
        "--chronolog-grapher-retire-on-stop-values",
        default=os.environ.get("CHRONOLOG_GRAPHER_RETIRE_ON_STOP", "1"),
    )
    parser.add_argument(
        "--chronolog-grapher-stop-retire-grace-us-values",
        default=os.environ.get("CHRONOLOG_GRAPHER_STOP_RETIRE_GRACE_US", "0"),
    )
    parser.add_argument(
        "--chronolog-grapher-stop-story-archive-drain-values",
        default=os.environ.get("CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN", "0"),
    )
    parser.add_argument(
        "--chronolog-grapher-stop-story-archive-drain-timeout-ms",
        default=os.environ.get("CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS", "30000"),
    )
    parser.add_argument(
        "--chronolog-grapher-extraction-threads",
        default=os.environ.get("CHRONOLOG_GRAPHER_EXTRACTION_THREADS", "2"),
    )
    parser.add_argument(
        "--chronolog-hdf5-archive-atomic-rename-values",
        default=os.environ.get("CHRONOLOG_HDF5_ARCHIVE_ATOMIC_RENAME", "0"),
    )
    parser.add_argument(
        "--chronolog-hdf5-archive-chunk-events",
        default=os.environ.get("CHRONOLOG_HDF5_ARCHIVE_CHUNK_EVENTS", "0"),
    )
    parser.add_argument(
        "--chronolog-hdf5-archive-layouts",
        default=os.environ.get("CHRONOLOG_HDF5_ARCHIVE_LAYOUT", "vlen"),
    )
    parser.add_argument(
        "--chronolog-raw-blob-preallocate-values",
        default=os.environ.get("CHRONOLOG_RAW_BLOB_PREALLOCATE", "0"),
    )
    parser.add_argument(
        "--chronolog-raw-blob-async-close-values",
        default=os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_CLOSE", "0"),
    )
    parser.add_argument(
        "--chronolog-raw-blob-async-publish-values",
        default=os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH", "0"),
    )
    parser.add_argument(
        "--chronolog-raw-blob-async-publish-threads",
        default=os.environ.get("CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH_THREADS", "1"),
    )
    parser.add_argument(
        "--chronolog-archive-range-event-counts",
        default=os.environ.get("CHRONOLOG_ARCHIVE_RANGE_EVENT_COUNT", "0"),
    )
    parser.add_argument(
        "--chronolog-hdf5-archive-reader-bin",
        default=os.environ.get(
            "CHRONOLOG_HDF5_ARCHIVE_READER_BIN",
            str(REPO_ROOT / ".agent" / "build-hdf5-unit" / "Release" / "test" / "unit" / "chrono-player" / "chronolog-test-player-hdf5-archive-reader"),
        ),
    )
    parser.add_argument("--chronolog-chrono-bench-barrier-modes", default="false")
    parser.add_argument("--chronolog-chrono-bench-shared-story-modes", default="false")
    parser.add_argument(
        "--chronolog-producer-outstanding-values",
        default=os.environ.get("CHRONOLOG_PRODUCER_OUTSTANDING", "1"),
    )
    parser.add_argument(
        "--chronolog-producer-batch-sizes",
        default=os.environ.get("CHRONOLOG_PRODUCER_BATCH_SIZE", "1"),
    )
    parser.add_argument(
        "--chronolog-producer-wait-modes",
        default=os.environ.get("CHRONOLOG_PRODUCER_WAIT_MODE", "auto"),
        help=(
            "Comma-separated ChronoLog producer wait policies: auto, per_event, "
            "bounded_outstanding, bounded_futures, bounded_api, bounded_per_keeper, after_loop."
        ),
    )
    parser.add_argument(
        "--chronolog-client-batch-keeper-selection-values",
        default=os.environ.get("CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION", "per_event"),
    )
    parser.add_argument(
        "--chronolog-client-keeper-time-bucket-ns-values",
        default=os.environ.get("CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS", "0"),
    )
    parser.add_argument(
        "--chronolog-client-parallel-tail-rpc-values",
        default=os.environ.get("CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC", "1"),
    )
    parser.add_argument(
        "--chronolog-client-keeper-cursor-drain-values",
        default=os.environ.get("CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN", "0"),
    )
    parser.add_argument(
        "--chronolog-client-keeper-cursor-drain-max-batches-values",
        default=os.environ.get("CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES", "0"),
    )
    parser.add_argument(
        "--chronolog-client-keeper-cursor-packed-batch-values",
        default=os.environ.get("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH", "0"),
    )
    parser.add_argument(
        "--chronolog-client-keeper-cursor-packed-bulk-values",
        default=os.environ.get("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK", "0"),
    )
    parser.add_argument(
        "--chronolog-client-keeper-cursor-packed-bulk-stream-values",
        default=os.environ.get("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM", "0"),
    )
    parser.add_argument(
        "--chronolog-client-keeper-cursor-packed-bulk-stream-max-batches-values",
        default=os.environ.get("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES", "0"),
    )
    parser.add_argument(
        "--chronolog-client-keeper-cursor-packed-bulk-buffer-bytes-values",
        default=os.environ.get("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES", "0"),
    )
    parser.add_argument(
        "--chronolog-mixed-tail-read-modes",
        default=os.environ.get("CHRONOLOG_MIXED_TAIL_READ_MODE", "full"),
        help="Comma-separated mixed_append_tail read modes: full, incremental, keeper_cursor.",
    )
    parser.add_argument(
        "--chronolog-mixed-tail-reader-start-modes",
        default=os.environ.get("CHRONOLOG_MIXED_TAIL_READER_START_MODE", "ready"),
        help="Comma-separated mixed_append_tail reader start modes: ready, after_writers_done.",
    )
    parser.add_argument(
        "--chronolog-client-execution-modes",
        default=os.environ.get("CHRONOLOG_CLIENT_EXECUTION_MODE", "sequential"),
    )
    parser.add_argument("--chronolog-keeper-journal-shards", default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_SHARDS", "1"))
    parser.add_argument(
        "--chronolog-keeper-journal-shard-policies",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_SHARD_POLICY", "mixed"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-placements",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_PLACEMENT", "shared"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-local-base",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_LOCAL_BASE", ""),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-fdatasync-batch-events",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_FDATASYNC_BATCH_EVENTS", "64"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-batch-writev-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_BATCH_WRITEV", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-move-batch-payloads-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-tail-batch-max-events",
        default=os.environ.get("CHRONOLOG_KEEPER_TAIL_BATCH_MAX_EVENTS", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-tail-batch-max-bytes",
        default=os.environ.get("CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-group-commit-wait-us",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_WAIT_US", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-group-commit-flush-events",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_EVENTS", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-group-commit-strict-flush-event-cap-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_STRICT_FLUSH_EVENT_CAP", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-group-commit-flush-bytes",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-group-commit-large-payload-bytes",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-group-commit-large-payload-flush-events",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS", "64"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-group-commit-flush-wait-us",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_WAIT_US", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-owner-drain-yields",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_OWNER_DRAIN_YIELDS", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-notify-owner-only-on-empty-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-append-stats-interval-events",
        default=os.environ.get("CHRONOLOG_KEEPER_APPEND_STATS_INTERVAL_EVENTS", "10000"),
    )
    parser.add_argument(
        "--chronolog-keeper-wal-drain-batch-events",
        default=os.environ.get("CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_EVENTS", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-wal-drain-batch-wait-us",
        default=os.environ.get("CHRONOLOG_KEEPER_WAL_DRAIN_BATCH_WAIT_US", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-async-drain-threads",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_ASYNC_DRAIN_THREADS", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-async-callback-dispatch-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-async-batch-completion-dispatch-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-callback-dispatch-threads",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-callback-batch-drain-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-callback-batch-drain-max-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-callback-batch-drain-min-payload-bytes-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-journal-durable-complete-before-publish-values",
        default=os.environ.get("CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-recording-margo-xstreams",
        default=os.environ.get("CHRONOLOG_KEEPER_RECORDING_MARGO_XSTREAMS", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-recording-margo-progress-thread-values",
        default=os.environ.get("CHRONOLOG_KEEPER_RECORDING_MARGO_PROGRESS_THREAD", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-recording-margo-handlers",
        default=os.environ.get("CHRONOLOG_KEEPER_RECORDING_MARGO_HANDLERS", "1"),
    )
    parser.add_argument(
        "--chronolog-grapher-recording-margo-xstreams",
        default=os.environ.get("CHRONOLOG_GRAPHER_RECORDING_MARGO_XSTREAMS", "1"),
    )
    parser.add_argument(
        "--chronolog-grapher-recording-margo-handlers",
        default=os.environ.get("CHRONOLOG_GRAPHER_RECORDING_MARGO_HANDLERS", "1"),
    )
    parser.add_argument(
        "--chronolog-grapher-direct-deserialize-values",
        default=os.environ.get("CHRONOLOG_GRAPHER_DIRECT_DESERIALIZE", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-drain-margo-progress-thread-values",
        default=os.environ.get("CHRONOLOG_KEEPER_DRAIN_MARGO_PROGRESS_THREAD", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-drain-margo-rpc-threads",
        default=os.environ.get("CHRONOLOG_KEEPER_DRAIN_MARGO_RPC_THREADS", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-extraction-threads",
        default=os.environ.get("CHRONOLOG_KEEPER_EXTRACTION_THREADS", "2"),
    )
    parser.add_argument(
        "--chronolog-keeper-direct-serialize-values",
        default=os.environ.get("CHRONOLOG_KEEPER_DIRECT_SERIALIZE", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-fast-wire-values",
        default=os.environ.get("CHRONOLOG_KEEPER_FAST_WIRE", "1"),
    )
    parser.add_argument(
        "--chronolog-keeper-stop-story-flush-drain-values",
        default=os.environ.get("CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN", "0"),
    )
    parser.add_argument(
        "--chronolog-keeper-stop-story-flush-drain-timeout-ms",
        default=os.environ.get("CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS", "30000"),
    )
    parser.add_argument(
        "--chronolog-visor-parallel-keeper-stop-values",
        default=os.environ.get("CHRONOLOG_VISOR_PARALLEL_KEEPER_STOP", "0"),
    )
    parser.add_argument(
        "--chronolog-deploy-stop-timeout-seconds",
        default=os.environ.get("CHRONOLOG_DEPLOY_STOP_TIMEOUT_SECONDS", "90"),
        help="ChronoLog service stop timeout used by the distributed run wrapper.",
    )
    parser.add_argument("--perf-bin", default=str(REPO_ROOT / "opt" / "perf" / "bin" / "perf"))
    parser.add_argument("--chronolog-perf-call-graph", default=os.environ.get("CHRONOLOG_PERF_CALL_GRAPH", "fp"))
    parser.add_argument("--mofka-partition-types", default="default")
    parser.add_argument("--mofka-storage-target-types", default="pmdk")
    parser.add_argument("--mofka-storage-target-sizes", default=os.environ.get("MOFKA_STORAGE_TARGET_SIZE", "67108864"))
    parser.add_argument("--mofka-producer-wait-modes", default="per_event")
    parser.add_argument("--mofka-producer-flush-modes", default="after_loop")
    parser.add_argument(
        "--mofka-precreate-storage-provider-values",
        default=os.environ.get("MOFKA_PRECREATE_STORAGE_PROVIDER", "yes"),
    )
    parser.add_argument("--mofka-group-ping-timeout-ms", default=os.environ.get("MOFKA_GROUP_PING_TIMEOUT_MS", "1000"))
    parser.add_argument(
        "--mofka-group-ping-interval-min-ms",
        default=os.environ.get("MOFKA_GROUP_PING_INTERVAL_MIN_MS", "1000"),
    )
    parser.add_argument(
        "--mofka-group-ping-interval-max-ms",
        default=os.environ.get("MOFKA_GROUP_PING_INTERVAL_MAX_MS", "1000"),
    )
    parser.add_argument(
        "--mofka-group-ping-max-timeouts",
        default=os.environ.get("MOFKA_GROUP_PING_MAX_TIMEOUTS", "3"),
    )
    parser.add_argument("--kafka-acks-values", default=os.environ.get("KAFKA_ACKS", "1"))
    parser.add_argument("--result-dir", default="")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def semantic_boundary(
    system: str,
    workflow: str,
    mofka_partition_type: str = "",
    mofka_storage_target_type: str = "",
    mofka_producer_wait_mode: str = "",
    mofka_producer_flush_mode: str = "",
    chronolog_completion_mode: str = "",
    kafka_acks: str = "1",
) -> dict[str, str]:
    """Describe what the current harness actually measures."""
    kafka_ack_value = str(kafka_acks or "1")
    if kafka_ack_value == "0":
        kafka_durability_boundary = "producer_no_broker_ack"
    elif kafka_ack_value in {"all", "-1"}:
        kafka_durability_boundary = "leader_and_in_sync_replicas_ack_rf1"
    else:
        kafka_durability_boundary = "broker_leader_ack_not_all_replicas"
    if system == "chronolog" and workflow == "append_throughput":
        if chronolog_completion_mode == "keeper_journal_fdatasync":
            return {
                "semantic_boundary": "append_keeper_journal_fdatasync",
                "append_ack_boundary": "record_event_return_after_keeper_journal_fdatasync",
                "durability_boundary": "keeper_local_journal_fdatasync",
                "read_path": "",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append returns after a Keeper-local journal record is fdatasync'ed; this is not archive durability.",
            }
        if chronolog_completion_mode == "keeper_journal_fdatasync_tail_only":
            return {
                "semantic_boundary": "append_keeper_journal_fdatasync_tail_only",
                "append_ack_boundary": "record_event_return_after_keeper_journal_fdatasync_no_timeline_ingest",
                "durability_boundary": "keeper_local_journal_fdatasync",
                "read_path": "keeper_local_journal_tail",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append returns after the Keeper-local journal record is fdatasync'ed and skips legacy timeline/archive ingestion. This is strict Keeper-local durable-tail evidence, not archive/storage readback.",
            }
        if chronolog_completion_mode == "keeper_journal_group_fdatasync":
            return {
                "semantic_boundary": "append_keeper_journal_periodic_fdatasync",
                "append_ack_boundary": "record_event_return_after_keeper_journal_write_group_fsync_periodic",
                "durability_boundary": "keeper_local_journal_periodic_fdatasync_not_per_record",
                "read_path": "",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append writes to the Keeper-local journal and fdatasyncs each shard only after the configured batch event count; non-boundary appends return before their batch is forced.",
            }
        if chronolog_completion_mode == "keeper_journal_group_fdatasync_early_ack":
            return {
                "semantic_boundary": "append_keeper_journal_periodic_fdatasync_early_ack",
                "append_ack_boundary": "record_event_return_after_keeper_journal_write_periodic_fsync_before_ingestion_drain",
                "durability_boundary": "keeper_local_journal_periodic_fdatasync_not_per_record",
                "read_path": "",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append writes to the Keeper-local journal, acknowledges after the local write/periodic fdatasync path, and feeds the old ingestion/timeline path after responding. Non-boundary records do not wait for per-record fdatasync.",
            }
        if chronolog_completion_mode == "keeper_journal_group_fdatasync_async_drain":
            return {
                "semantic_boundary": "append_keeper_journal_periodic_fdatasync_async_drain",
                "append_ack_boundary": "record_event_return_after_keeper_journal_write_periodic_fsync_and_memory_drain_enqueue",
                "durability_boundary": "keeper_local_journal_periodic_fdatasync_not_per_record",
                "read_path": "",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append writes to the Keeper-local journal, acknowledges after the local write/periodic fdatasync path, and enqueues a memory copy for legacy ingestion. Non-boundary records do not wait for per-record fdatasync.",
            }
        if chronolog_completion_mode == "keeper_journal_group_fdatasync_wal_drain":
            return {
                "semantic_boundary": "append_keeper_journal_periodic_fdatasync_wal_drain",
                "append_ack_boundary": "record_event_return_after_keeper_journal_write_periodic_fsync_and_wal_cursor_enqueue",
                "durability_boundary": "keeper_local_journal_periodic_fdatasync_not_per_record",
                "read_path": "",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append writes to the Keeper-local journal, acknowledges after the local write/periodic fdatasync path, and enqueues a WAL cursor for legacy ingestion. Non-boundary records do not wait for per-record fdatasync.",
            }
        if chronolog_completion_mode == "keeper_journal_group_fdatasync_tail_only":
            return {
                "semantic_boundary": "append_keeper_journal_periodic_fdatasync_tail_only",
                "append_ack_boundary": "record_event_return_after_keeper_journal_write_periodic_fsync_no_timeline_ingest",
                "durability_boundary": "keeper_local_journal_periodic_fdatasync_not_per_record",
                "read_path": "keeper_local_journal_tail",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append writes to the Keeper-local journal and skips legacy ingestion. Non-boundary records do not wait for per-record fdatasync; use restart/readback workflows for recovery evidence.",
            }
        if chronolog_completion_mode == "keeper_journal_group_commit_tail_only":
            return {
                "semantic_boundary": "append_keeper_journal_group_commit_tail_only",
                "append_ack_boundary": "record_event_return_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest",
                "durability_boundary": "keeper_local_journal_group_commit_fdatasync",
                "read_path": "keeper_local_journal_tail",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append is served by a single Keeper journal owner per shard; the owner writes a batch, fdatasyncs once, then completes all appends in that batch. This is Keeper-local durable-tail evidence, not archive/storage readback.",
            }
        if chronolog_completion_mode == "keeper_journal_group_commit_deferred_tail_only":
            return {
                "semantic_boundary": "append_keeper_journal_group_commit_deferred_rpc_tail_only",
                "append_ack_boundary": "record_event_response_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest",
                "durability_boundary": "keeper_local_journal_group_commit_fdatasync",
                "read_path": "keeper_local_journal_tail",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog append RPC response is deferred until the single-writer Keeper journal owner writes a batch, fdatasyncs once, then responds to all appends in that batch. This is Keeper-local durable-tail evidence, not archive/storage readback.",
            }
        return {
            "semantic_boundary": "append_enqueue_async",
            "append_ack_boundary": "chrono_bench_record_event_return",
            "durability_boundary": "not_proven_durable",
            "read_path": "",
            "storage_backend": "chronolog_keeper_grapher_archive",
            "semantic_notes": "Compare only to similarly labeled append enqueue/ack boundaries. For multi-client shared-story ChronoLog rows, barrier=true is required so one rank cannot release/destroy while another rank is still writing.",
        }
    if system == "chronolog" and workflow == "append_latency":
        return {
            "semantic_boundary": "append_api_latency",
            "append_ack_boundary": "StoryHandle.log_event_return",
            "durability_boundary": "not_proven_durable",
            "read_path": "",
            "storage_backend": "chronolog_keeper_grapher_archive",
            "semantic_notes": "Python client latency measures log_event return time, not archive persistence.",
        }
    if system == "chronolog" and workflow == "range_retrieval":
        return {
            "semantic_boundary": "range_retrieval_live_tail_or_archive_fallback",
            "append_ack_boundary": "StoryHandle.log_event_return",
            "durability_boundary": "path_classified_by_metrics",
            "read_path": "keeper_live_tail_first_then_archive",
            "storage_backend": "chronolog_keeper_grapher_archive",
            "semantic_notes": "Use live_tail_succeeded, archive artifacts, total bytes, and profiler evidence to classify hot tail versus archive-backed range.",
        }
    if system == "chronolog" and workflow == "archive_range_retrieval":
        return {
            "semantic_boundary": "archive_storage_range_retrieval",
            "append_ack_boundary": "StoryHandle.log_event_return_then_release_before_archive_wait",
            "durability_boundary": "archive_file_event_count_then_chronoplayer_hdf5_readback",
            "read_path": "chronoplayer_hdf5_archive_reader",
            "storage_backend": "chronolog_grapher_hdf5_archive",
            "semantic_notes": "ChronoLog writes events, waits for archive event-count evidence, then reads a timestamp subrange directly through ChronoPlayer HDF5ArchiveReadingAgent. This row must not be compared as live Keeper-tail retrieval.",
        }
    if system == "chronolog" and workflow == "mixed_append_tail":
        return {
            "semantic_boundary": "mixed_append_with_concurrent_keeper_tail_reads",
            "append_ack_boundary": "StoryHandle.log_event_return",
            "durability_boundary": "path_classified_by_metrics",
            "read_path": "concurrent_ReplayStory_keeper_tail",
            "storage_backend": "chronolog_keeper_journal_or_timeline",
            "semantic_notes": "One writer process appends while one reader process repeatedly replays the same acquired story through the configured Keeper tail source. Use tail counters and journal parser fields before comparing.",
        }
    if system == "chronolog" and workflow == "keeper_restart_recovery":
        if chronolog_completion_mode == "keeper_journal_fdatasync_tail_only":
            return {
                "semantic_boundary": "keeper_restart_recovery_fdatasync_tail_only_journal_tail",
                "append_ack_boundary": "StoryHandle.log_event_return_after_keeper_journal_fdatasync_no_timeline_ingest",
                "durability_boundary": "keeper_local_journal_fdatasync_recovered_after_keeper_restart",
                "read_path": "keeper_tail_rpc_recovered_journal_index",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog writes through normal client RPCs with strict Keeper-local journal fdatasync tail-only semantics, skips legacy timeline/archive ingestion, restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes. This is not archive/storage readback evidence.",
            }
        if chronolog_completion_mode == "keeper_journal_group_fdatasync":
            return {
                "semantic_boundary": "keeper_restart_recovery_periodic_fdatasync_journal_tail",
                "append_ack_boundary": "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync",
                "durability_boundary": "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart",
                "read_path": "keeper_tail_rpc_recovered_journal_index",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync batching, restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes. Append return is not per-record fsync wait for non-boundary records; the durability evidence is the restart/readback gate.",
            }
        if chronolog_completion_mode == "keeper_journal_group_fdatasync_wal_drain":
            return {
                "semantic_boundary": "keeper_restart_recovery_periodic_fdatasync_wal_drain_journal_tail",
                "append_ack_boundary": "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync_and_wal_cursor_enqueue",
                "durability_boundary": "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart",
                "read_path": "keeper_tail_rpc_recovered_journal_index",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync batching and WAL-cursor async drain semantics, restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes. Append return is not per-record fsync wait for non-boundary records; the durability evidence is the restart/readback gate.",
            }
        if chronolog_completion_mode == "keeper_journal_group_fdatasync_tail_only":
            return {
                "semantic_boundary": "keeper_restart_recovery_periodic_fdatasync_tail_only_journal_tail",
                "append_ack_boundary": "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync_no_timeline_ingest",
                "durability_boundary": "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart",
                "read_path": "keeper_tail_rpc_recovered_journal_index",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync tail-only semantics, skips the legacy timeline/archive consumer, restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes. Append return is not per-record fsync wait for non-boundary records; the durability evidence is the restart/readback gate. This is not archive/storage readback evidence.",
            }
        if chronolog_completion_mode == "keeper_journal_group_commit_tail_only":
            return {
                "semantic_boundary": "keeper_restart_recovery_group_commit_tail_only_journal_tail",
                "append_ack_boundary": "StoryHandle.log_event_return_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest",
                "durability_boundary": "keeper_local_journal_group_commit_fdatasync_recovered_after_keeper_restart",
                "read_path": "keeper_tail_rpc_recovered_journal_index",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog writes through normal client RPCs to a single Keeper journal owner per shard. The owner writes a batch, fdatasyncs once, then completes all appends in that batch; legacy timeline/archive ingestion is skipped. Keepers are restarted and the same story is read through recovered Keeper journal indexes. This is not archive/storage readback evidence.",
            }
        if chronolog_completion_mode == "keeper_journal_group_commit_deferred_tail_only":
            return {
                "semantic_boundary": "keeper_restart_recovery_group_commit_deferred_rpc_tail_only_journal_tail",
                "append_ack_boundary": "StoryHandle.log_event_return_after_deferred_keeper_journal_group_commit_fdatasync_no_timeline_ingest",
                "durability_boundary": "keeper_local_journal_group_commit_fdatasync_recovered_after_keeper_restart",
                "read_path": "keeper_tail_rpc_recovered_journal_index",
                "storage_backend": "chronolog_keeper_local_journal",
                "semantic_notes": "ChronoLog writes through normal client RPCs to a single Keeper journal owner per shard. The RPC response is deferred until the owner writes a batch and completes a covering fdatasync; legacy timeline/archive ingestion is skipped. Keepers are restarted and the same story is read through recovered Keeper journal indexes. This is not archive/storage readback evidence.",
            }
        return {
            "semantic_boundary": "keeper_restart_recovery_fdatasync_journal_tail",
            "append_ack_boundary": "StoryHandle.log_event_return_after_keeper_journal_fdatasync",
            "durability_boundary": "keeper_local_journal_fdatasync_recovered_after_keeper_restart",
            "read_path": "keeper_tail_rpc_recovered_journal_index",
            "storage_backend": "chronolog_keeper_local_journal",
            "semantic_notes": "ChronoLog writes through normal client RPCs, restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes.",
        }
    if system == "chronolog" and workflow == "append_durable":
        return {
            "semantic_boundary": "append_archive_readback_storage_completion",
            "append_ack_boundary": "StoryHandle.log_event_return_then_ReleaseStory",
            "durability_boundary": "archive_file_event_count_and_readback",
            "read_path": "archive_file_count_plus_ReplayStory_readback",
            "storage_backend": "chronolog_keeper_grapher_hdf5_archive",
            "semantic_notes": "Durable ChronoLog mode includes append, story release, HDF5 archive event-count evidence, and replay/readback validation. The raw_blob layout stores payload bytes in an archive sidecar and HDF5 metadata/index in the archive file.",
        }
    if system == "kafka" and workflow in {"append_throughput", "append_latency"}:
        return {
            "semantic_boundary": f"append_ack_acks_{kafka_ack_value}",
            "append_ack_boundary": f"kafka_producer_perf_test_acks_{kafka_ack_value}",
            "durability_boundary": kafka_durability_boundary,
            "read_path": "",
            "storage_backend": "kafka_log_dirs",
            "semantic_notes": f"Kafka producer perf uses acks={kafka_ack_value} and replication-factor=1 in this harness.",
        }
    if system == "kafka" and workflow == "range_retrieval":
        return {
            "semantic_boundary": f"consumer_catchup_read_after_append_acks_{kafka_ack_value}",
            "append_ack_boundary": f"kafka_producer_perf_test_acks_{kafka_ack_value}",
            "durability_boundary": kafka_durability_boundary,
            "read_path": "kafka_consumer_perf_from_topic",
            "storage_backend": "kafka_log_dirs",
            "semantic_notes": f"Harness appends first with Kafka acks={kafka_ack_value}, then measures consumer catch-up read; replication-factor=1.",
        }
    if system == "mofka" and mofka_partition_type == "memory" and workflow in {"append_throughput", "append_latency"}:
        return {
            "semantic_boundary": f"append_{mofka_producer_wait_mode or 'per_event'}_{mofka_producer_flush_mode or 'after_loop'}_memory_partition",
            "append_ack_boundary": f"producer_{mofka_producer_wait_mode or 'per_event'}",
            "durability_boundary": "mofka_memory_partition_not_durable",
            "read_path": "",
            "storage_backend": "mofka_memory_partition",
            "semantic_notes": "Mofka memory partition benchmark with explicit producer wait/flush knobs; it is not a storage durability boundary.",
        }
    if system == "mofka" and mofka_partition_type == "memory" and workflow == "range_retrieval":
        return {
            "semantic_boundary": f"consumer_pull_wait_after_append_{mofka_producer_wait_mode or 'per_event'}_{mofka_producer_flush_mode or 'after_loop'}_memory_partition",
            "append_ack_boundary": f"producer_{mofka_producer_wait_mode or 'per_event'}",
            "durability_boundary": "mofka_memory_partition_not_durable",
            "read_path": "mofka_consumer_pull_wait",
            "storage_backend": "mofka_memory_partition",
            "semantic_notes": "Harness appends into a Mofka memory partition with explicit producer wait/flush knobs, then measures consumer pull().wait().",
        }
    if system == "mofka" and workflow in {"append_throughput", "append_latency"}:
        return {
            "semantic_boundary": f"append_{mofka_producer_wait_mode or 'per_event'}_{mofka_producer_flush_mode or 'after_loop'}_storage_backed",
            "append_ack_boundary": f"producer_{mofka_producer_wait_mode or 'per_event'}",
            "durability_boundary": f"mofka_default_partition_warabi_{mofka_storage_target_type or 'pmdk'}_target",
            "read_path": "",
            "storage_backend": f"mofka_yokan_warabi_{mofka_storage_target_type or 'pmdk'}",
            "semantic_notes": "Mofka default partition benchmark with explicit producer wait/flush knobs.",
        }
    if system == "mofka" and workflow == "range_retrieval":
        return {
            "semantic_boundary": f"consumer_pull_wait_after_append_{mofka_producer_wait_mode or 'per_event'}_{mofka_producer_flush_mode or 'after_loop'}_storage_backed",
            "append_ack_boundary": f"producer_{mofka_producer_wait_mode or 'per_event'}",
            "durability_boundary": f"mofka_default_partition_warabi_{mofka_storage_target_type or 'pmdk'}_target",
            "read_path": "mofka_consumer_pull_wait",
            "storage_backend": f"mofka_yokan_warabi_{mofka_storage_target_type or 'pmdk'}",
            "semantic_notes": "Harness appends with explicit producer wait/flush knobs, then measures consumer pull().wait().",
        }
    return {
        "semantic_boundary": "unknown",
        "append_ack_boundary": "unknown",
        "durability_boundary": "unknown",
        "read_path": "",
        "storage_backend": "unknown",
        "semantic_notes": "No semantic classification is defined for this system/workflow pair.",
    }


def mofka_backend_modes(
    partition_types: list[str],
    storage_target_types: list[str],
    producer_wait_modes: list[str],
    producer_flush_modes: list[str],
    precreate_storage_provider_modes: list[str],
) -> list[tuple[str, str, str, str, str]]:
    modes: list[tuple[str, str, str, str, str]] = []
    for (
        partition_type,
        storage_target_type,
        producer_wait_mode,
        producer_flush_mode,
        precreate_storage_provider,
    ) in itertools.product(
        partition_types,
        storage_target_types,
        producer_wait_modes,
        producer_flush_modes,
        precreate_storage_provider_modes,
    ):
        if partition_type == "memory" and storage_target_type != "memory":
            continue
        if partition_type == "default" and storage_target_type == "memory":
            continue
        modes.append(
            (
                partition_type,
                storage_target_type,
                producer_wait_mode,
                producer_flush_mode,
                precreate_storage_provider,
            )
        )
    return modes


def mofka_cli_flush_mode(value: str) -> str:
    """Normalize matrix labels to the Mofka benchmark CLI vocabulary."""
    return "none" if value == "no_flush" else value


def run_dir(args: argparse.Namespace) -> Path:
    if args.result_dir:
        root = Path(args.result_dir).resolve()
    else:
        root = RESULTS_ROOT / timestamp()
    root.mkdir(parents=True, exist_ok=True)
    return root


def command_for(run: dict[str, Any], child_dir: Path, args: argparse.Namespace) -> list[str]:
    system = run["system"]
    workflow = run["workflow"]
    script = SYSTEM_SCRIPTS[system]
    base = [str(script), "--result-dir", str(child_dir)]
    if system == "mofka":
        base.extend(
            [
                "--node-count",
                str(run["node_count"]),
                "--operation-count",
                str(run["operation_count"]),
                "--message-size-bytes",
                str(run["message_size_bytes"]),
                "--client-count",
                str(run["client_count"]),
                "--workflow",
                workflow,
                "--partition-type",
                run["mofka_partition_type"],
                "--storage-target-type",
                run["mofka_storage_target_type"],
                "--storage-target-size",
                str(run["mofka_storage_target_size"]),
                "--producer-wait-mode",
                run["mofka_producer_wait_mode"],
                "--producer-flush-mode",
                mofka_cli_flush_mode(run["mofka_producer_flush_mode"]),
                "--precreate-storage-provider",
                run["mofka_precreate_storage_provider"],
                "--group-ping-timeout-ms",
                str(run["mofka_group_ping_timeout_ms"]),
                "--group-ping-interval-min-ms",
                str(run["mofka_group_ping_interval_min_ms"]),
                "--group-ping-interval-max-ms",
                str(run["mofka_group_ping_interval_max_ms"]),
                "--group-ping-max-timeouts",
                str(run["mofka_group_ping_max_timeouts"]),
                "--deployment-mode",
                "bare_metal",
                "--slurm-partition",
                args.partition,
                "--slurm-time",
                args.slurm_time,
            ]
        )
        if args.nodelist:
            base.extend(["--slurm-nodelist", args.nodelist])
        return base

    base.extend(
        [
            "--partition",
            args.partition,
            "--node-count",
            str(run["node_count"]),
            "--operation-count",
            str(run["operation_count"]),
            "--message-size-bytes",
            str(run["message_size_bytes"]),
            "--client-count",
            str(run["client_count"]),
        ]
    )
    if system in {"chronolog", "kafka"}:
        base.extend(["--workflow", workflow, "--slurm-time", args.slurm_time])
    if system == "chronolog":
        base.extend(["--install-dir", args.chronolog_install_dir, "--profile-mode", args.chronolog_profile_mode])
        base.extend(["--completion-mode", run["chronolog_completion_mode"]])
        base.extend(["--keeper-journal-shards", str(run["chronolog_keeper_journal_shards"])])
        base.extend(["--keeper-journal-shard-policy", str(run["chronolog_keeper_journal_shard_policy"])])
        base.extend(["--keeper-journal-placement", str(run["chronolog_keeper_journal_placement"])])
        base.extend(["--keeper-journal-local-base", str(run["chronolog_keeper_journal_local_base"])])
        base.extend(
            [
                "--keeper-journal-fdatasync-batch-events",
                str(run["chronolog_keeper_journal_fdatasync_batch_events"]),
            ]
        )
        base.extend(["--keeper-journal-batch-writev", str(run["chronolog_keeper_journal_batch_writev"])])
        base.extend(
            [
                "--keeper-journal-move-batch-payloads",
                str(run["chronolog_keeper_journal_move_batch_payloads"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-group-commit-flush-events",
                str(run["chronolog_keeper_journal_group_commit_flush_events"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-group-commit-strict-flush-event-cap",
                str(run["chronolog_keeper_journal_group_commit_strict_flush_event_cap"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-group-commit-flush-bytes",
                str(run["chronolog_keeper_journal_group_commit_flush_bytes"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-group-commit-large-payload-bytes",
                str(run["chronolog_keeper_journal_group_commit_large_payload_bytes"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-group-commit-large-payload-flush-events",
                str(run["chronolog_keeper_journal_group_commit_large_payload_flush_events"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-group-commit-wait-us",
                str(run["chronolog_keeper_journal_group_commit_wait_us"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-group-commit-flush-wait-us",
                str(run["chronolog_keeper_journal_group_commit_flush_wait_us"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-owner-drain-yields",
                str(run["chronolog_keeper_journal_owner_drain_yields"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-notify-owner-only-on-empty",
                str(run["chronolog_keeper_journal_notify_owner_only_on_empty"]),
            ]
        )
        base.extend(
            [
                "--keeper-append-stats-interval-events",
                str(run["chronolog_keeper_append_stats_interval_events"]),
            ]
        )
        base.extend(["--keeper-wal-drain-batch-events", str(run["chronolog_keeper_wal_drain_batch_events"])])
        base.extend(["--keeper-wal-drain-batch-wait-us", str(run["chronolog_keeper_wal_drain_batch_wait_us"])])
        base.extend(
            [
                "--keeper-journal-async-drain-threads",
                str(run["chronolog_keeper_journal_async_drain_threads"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-async-batch-completion-dispatch",
                str(run["chronolog_keeper_journal_async_batch_completion_dispatch"]),
            ]
        )
        base.extend(["--keeper-recording-margo-xstreams", str(run["chronolog_keeper_recording_margo_xstreams"])])
        base.extend(
            [
                "--keeper-recording-margo-progress-thread",
                str(run["chronolog_keeper_recording_margo_progress_thread"]),
            ]
        )
        base.extend(["--keeper-recording-margo-handlers", str(run["chronolog_keeper_recording_margo_handlers"])])
        base.extend(["--grapher-recording-margo-xstreams", str(run["chronolog_grapher_recording_margo_xstreams"])])
        base.extend(["--grapher-recording-margo-handlers", str(run["chronolog_grapher_recording_margo_handlers"])])
        base.extend(["--grapher-direct-deserialize", str(run["chronolog_grapher_direct_deserialize"])])
        base.extend(
            [
                "--keeper-drain-margo-progress-thread",
                str(run["chronolog_keeper_drain_margo_progress_thread"]),
            ]
        )
        base.extend(["--keeper-drain-margo-rpc-threads", str(run["chronolog_keeper_drain_margo_rpc_threads"])])
        base.extend(["--keeper-extraction-threads", str(run["chronolog_keeper_extraction_threads"])])
        base.extend(["--keeper-direct-serialize", str(run["chronolog_keeper_direct_serialize"])])
        base.extend(["--keeper-fast-wire", str(run["chronolog_keeper_fast_wire"])])
        base.extend(["--keeper-journal-callback-batch-drain", str(run["chronolog_keeper_journal_callback_batch_drain"])])
        base.extend(
            [
                "--keeper-journal-callback-batch-drain-max",
                str(run["chronolog_keeper_journal_callback_batch_drain_max"]),
            ]
        )
        base.extend(
            [
                "--keeper-journal-callback-batch-drain-min-payload-bytes",
                str(run["chronolog_keeper_journal_callback_batch_drain_min_payload_bytes"]),
            ]
        )
        base.extend(["--keeper-data-collection-streams", str(run["chronolog_keeper_data_collection_streams"])])
        base.extend(
            [
                "--keeper-data-collection-threads-per-stream",
                str(run["chronolog_keeper_data_collection_threads_per_stream"]),
            ]
        )
        base.extend(["--grapher-data-collection-streams", str(run["chronolog_grapher_data_collection_streams"])])
        base.extend(
            [
                "--grapher-data-collection-threads-per-stream",
                str(run["chronolog_grapher_data_collection_threads_per_stream"]),
            ]
        )
        base.extend(
            [
                "--keeper-stop-story-flush-drain",
                str(run["chronolog_keeper_stop_story_flush_drain"]),
            ]
        )
        base.extend(
            [
                "--keeper-stop-story-flush-drain-timeout-ms",
                str(run["chronolog_keeper_stop_story_flush_drain_timeout_ms"]),
            ]
        )
        base.extend(["--visor-parallel-keeper-stop", str(run["chronolog_visor_parallel_keeper_stop"])])
        base.extend(["--data-collection-poll-interval-us", str(run["chronolog_data_collection_poll_interval_us"])])
        if run.get("chronolog_archive_event_count_poll_interval_seconds", "") != "":
            base.extend(
                [
                    "--archive-event-count-poll-interval-seconds",
                    str(run["chronolog_archive_event_count_poll_interval_seconds"]),
                ]
            )
        if run.get("chronolog_archive_readback_mode", "") != "":
            base.extend(["--archive-readback-mode", str(run["chronolog_archive_readback_mode"])])
        if run["chronolog_grapher_inactive_story_delay_seconds"] != "":
            base.extend(
                [
                    "--grapher-inactive-story-delay-seconds",
                    str(run["chronolog_grapher_inactive_story_delay_seconds"]),
                ]
            )
        base.extend(["--chrono-bench-barrier", str(run["chronolog_chrono_bench_barrier"])])
        base.extend(["--chrono-bench-shared-story", str(run["chronolog_chrono_bench_shared_story"])])
        base.extend(["--chronolog-producer-outstanding", str(run["chronolog_producer_outstanding"])])
        base.extend(["--chronolog-producer-batch-size", str(run["chronolog_producer_batch_size"])])
        base.extend(["--chronolog-producer-wait-mode", str(run["chronolog_producer_wait_policy"])])
        base.extend(
            [
                "--chronolog-client-batch-keeper-selection",
                str(run["chronolog_client_batch_keeper_selection"]),
            ]
        )
        base.extend(
            [
                "--chronolog-client-keeper-time-bucket-ns",
                str(run["chronolog_client_keeper_time_bucket_ns"]),
            ]
        )
        base.extend(["--chronolog-client-parallel-tail-rpc", str(run["chronolog_client_parallel_tail_rpc"])])
        base.extend(
            [
                "--chronolog-client-keeper-cursor-drain",
                str(run["chronolog_client_keeper_cursor_drain"]),
            ]
        )
        base.extend(
            [
                "--chronolog-client-keeper-cursor-drain-max-batches",
                str(run["chronolog_client_keeper_cursor_drain_max_batches"]),
            ]
        )
        base.extend(
            [
                "--chronolog-client-keeper-cursor-packed-batch",
                str(run["chronolog_client_keeper_cursor_packed_batch"]),
            ]
        )
        base.extend(
            [
                "--chronolog-client-keeper-cursor-packed-bulk",
                str(run["chronolog_client_keeper_cursor_packed_bulk"]),
            ]
        )
        base.extend(
            [
                "--chronolog-client-keeper-cursor-packed-bulk-stream",
                str(run["chronolog_client_keeper_cursor_packed_bulk_stream"]),
            ]
        )
        base.extend(
            [
                "--chronolog-client-keeper-cursor-packed-bulk-stream-max-batches",
                str(run["chronolog_client_keeper_cursor_packed_bulk_stream_max_batches"]),
            ]
        )
        base.extend(
            [
                "--chronolog-client-keeper-cursor-packed-bulk-buffer-bytes",
                str(run["chronolog_client_keeper_cursor_packed_bulk_buffer_bytes"]),
            ]
        )
        if workflow == "mixed_append_tail":
            base.extend(["--chronolog-mixed-tail-read-mode", str(run["chronolog_mixed_tail_read_mode"])])
            base.extend(
                [
                    "--chronolog-mixed-tail-reader-start-mode",
                    str(run["chronolog_mixed_tail_reader_start_mode"]),
                ]
            )
        base.extend(["--chronolog-client-execution-mode", str(run["chronolog_client_execution_mode"])])
        base.extend(["--keeper-tail-batch-max-events", str(run["chronolog_keeper_tail_batch_max_events"])])
        base.extend(["--keeper-tail-batch-max-bytes", str(run["chronolog_keeper_tail_batch_max_bytes"])])
        base.extend(["--grapher-retire-on-stop", str(run["chronolog_grapher_retire_on_stop"])])
        base.extend(["--grapher-stop-retire-grace-us", str(run["chronolog_grapher_stop_retire_grace_us"])])
        base.extend(
            [
                "--grapher-stop-story-archive-drain",
                str(run["chronolog_grapher_stop_story_archive_drain"]),
            ]
        )
        base.extend(
            [
                "--grapher-stop-story-archive-drain-timeout-ms",
                str(run["chronolog_grapher_stop_story_archive_drain_timeout_ms"]),
            ]
        )
        base.extend(["--grapher-extraction-threads", str(run["chronolog_grapher_extraction_threads"])])
        base.extend(["--hdf5-archive-atomic-rename", str(run["chronolog_hdf5_archive_atomic_rename"])])
        base.extend(["--hdf5-archive-chunk-events", str(run["chronolog_hdf5_archive_chunk_events"])])
        base.extend(["--hdf5-archive-layout", str(run["chronolog_hdf5_archive_layout"])])
        base.extend(["--raw-blob-preallocate", str(run["chronolog_raw_blob_preallocate"])])
        base.extend(["--raw-blob-async-close", str(run["chronolog_raw_blob_async_close"])])
        base.extend(["--raw-blob-async-publish", str(run["chronolog_raw_blob_async_publish"])])
        base.extend(["--raw-blob-async-publish-threads", str(run["chronolog_raw_blob_async_publish_threads"])])
        if workflow == "archive_range_retrieval":
            base.extend(["--archive-range-event-count", str(run["chronolog_archive_range_event_count"])])
            base.extend(["--hdf5-archive-reader-bin", str(args.chronolog_hdf5_archive_reader_bin)])
        if args.nodelist:
            base.extend(["--nodelist", args.nodelist])
        if args.chronolog_profile_mode == "perf":
            base.extend(["--perf-bin", args.perf_bin])
            base.extend(["--perf-call-graph", args.chronolog_perf_call_graph])
    if system == "kafka":
        base.extend(["--acks", str(run["kafka_acks"])])
        if args.nodelist:
            base.extend(["--slurm-nodelist", args.nodelist])
    return base


def metrics_path(system: str, child_dir: Path) -> Path:
    return child_dir / system / "metrics.json"


def parse_log_timestamp(line: str) -> float | None:
    match = re.match(r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]", line)
    if not match:
        return None
    try:
        return datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
    except ValueError:
        return None


def elapsed_since(reference: Any, timestamp: Any) -> float | None:
    if reference in {None, ""} or timestamp in {None, ""}:
        return None
    try:
        return float(timestamp) - float(reference)
    except (TypeError, ValueError):
        return None


def add_microsecond_sample_stats(row: dict[str, Any], prefix: str, samples: list[float]) -> None:
    row[f"{prefix}_us"] = sum(samples)
    row[f"{prefix}_avg_us"] = sum(samples) / len(samples) if samples else None
    row[f"{prefix}_max_us"] = max(samples) if samples else None
    row[f"{prefix}_min_us"] = min(samples) if samples else None


def augment_chronolog_archive_log_metrics(row: dict[str, Any], metrics_file: Path) -> None:
    if row.get("system") != "chronolog" or row.get("workflow") not in {
        "append_throughput",
        "append_durable",
        "archive_range_retrieval",
    }:
        return
    log_dir = metrics_file.parent / "logs"
    if not log_dir.exists():
        return
    stage_times: dict[str, float | None] = {
        "grapher_start_recording": None,
        "grapher_stop_recording": None,
        "grapher_pipeline_scheduled": None,
        "grapher_pipeline_finalized": None,
        "grapher_hdf5_processing": None,
        "grapher_hdf5_written": None,
    }
    story_id = str(row.get("story_id") or "")
    aggregate_all_stories = row.get("workflow") in {"append_throughput", "archive_range_retrieval"}
    story_pattern = r"\d+" if aggregate_all_stories else re.escape(story_id)
    for path in sorted(log_dir.glob("chrono-grapher-*.log*")):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            timestamp = parse_log_timestamp(line)
            if timestamp is None:
                continue
            start_match = re.search(r"Starting Story Recording: StoryName=.*?, StoryID=(\d+)", line)
            if start_match and not story_id:
                story_id = start_match.group(1)
                stage_times["grapher_start_recording"] = stage_times["grapher_start_recording"] or timestamp
                continue
            if story_id and not aggregate_all_stories and story_id not in line:
                continue
            if "Starting Story Recording:" in line:
                stage_times["grapher_start_recording"] = stage_times["grapher_start_recording"] or timestamp
            elif "Stopping Story Recording:" in line:
                stage_times["grapher_stop_recording"] = stage_times["grapher_stop_recording"] or timestamp
            elif "Scheduled pipeline to retire" in line:
                stage_times["grapher_pipeline_scheduled"] = stage_times["grapher_pipeline_scheduled"] or timestamp
            elif "Finalized ingestion handle" in line:
                stage_times["grapher_pipeline_finalized"] = stage_times["grapher_pipeline_finalized"] or timestamp
            elif "HDF5FileChunkExtractor" in line and "processing chunk" in line:
                stage_times["grapher_hdf5_processing"] = stage_times["grapher_hdf5_processing"] or timestamp
            elif "HDF5FileChunkExtractor" in line and "StoryChunk written to file" in line:
                stage_times["grapher_hdf5_written"] = stage_times["grapher_hdf5_written"] or timestamp

    release_returned_at = row.get("release_returned_epoch_seconds")
    row["story_id"] = story_id
    for stage, timestamp in stage_times.items():
        row[f"release_to_{stage}_seconds"] = elapsed_since(release_returned_at, timestamp)
    row["grapher_finalize_to_hdf5_written_seconds"] = elapsed_since(
        stage_times.get("grapher_pipeline_finalized"), stage_times.get("grapher_hdf5_written")
    )
    if stage_times:
        row["grapher_archive_stage_epoch_seconds"] = stage_times

    drain_first = None
    drain_last = None
    drain_count = 0
    keeper_flush_start_first = None
    keeper_flush_complete_last = None
    keeper_flush_durations: dict[str, float] = {}
    keeper_flush_starts: dict[str, float] = {}
    keeper_flush_finalize_us: list[float] = []
    keeper_flush_wait_us: list[float] = []
    keeper_flush_total_us: list[float] = []
    keeper_drain_profile_count = 0
    keeper_drain_profile_events = 0
    keeper_drain_profile_bytes = 0
    keeper_drain_profile_serialization_us: list[float] = []
    keeper_drain_profile_rpc_us: list[float] = []
    keeper_drain_profile_total_us: list[float] = []
    rdma_transfer_profile_count = 0
    rdma_transfer_profile_bytes = 0
    rdma_transfer_profile_transferred_bytes = 0
    rdma_transfer_profile_expose_us = 0.0
    rdma_transfer_profile_remote_call_us = 0.0
    rdma_transfer_profile_total_us = 0.0
    grapher_receive_profile_count = 0
    grapher_receive_profile_events = 0
    grapher_receive_profile_bytes = 0
    grapher_receive_profile_allocation_us: list[float] = []
    grapher_receive_profile_local_bulk_expose_us: list[float] = []
    grapher_receive_profile_bulk_transfer_us: list[float] = []
    grapher_receive_profile_deserialization_us: list[float] = []
    grapher_receive_profile_ingestion_us: list[float] = []
    grapher_receive_profile_response_us: list[float] = []
    grapher_receive_profile_total_before_response_us: list[float] = []
    grapher_receive_profile_total_with_response_us: list[float] = []
    grapher_archive_drain_count = 0
    grapher_archive_drain_finalize_us: list[float] = []
    grapher_archive_drain_wait_us: list[float] = []
    grapher_archive_drain_queued_nonzero_count = 0
    grapher_archive_drain_inflight_nonzero_count = 0
    grapher_stop_retire_profile_count = 0
    grapher_stop_retire_profile_async_count = 0
    grapher_stop_retire_initial_lock_wait_us: list[float] = []
    grapher_stop_retire_initial_lock_hold_us: list[float] = []
    grapher_stop_retire_completion_wait_us: list[float] = []
    grapher_stop_retire_collect_erase_us: list[float] = []
    grapher_stop_retire_async_lock_wait_us: list[float] = []
    grapher_stop_retire_finalize_us: list[float] = []
    grapher_stop_retire_archive_drain_wait_us: list[float] = []
    grapher_stop_retire_total_us: list[float] = []
    grapher_story_pipeline_merge_profile_count = 0
    grapher_story_pipeline_merge_profile_source_events = 0
    grapher_story_pipeline_merge_profile_merged_events = 0
    grapher_story_pipeline_merge_profile_remaining_events = 0
    grapher_story_pipeline_merge_profile_total_us = 0.0
    grapher_hdf5_write_profile_count = 0
    grapher_hdf5_write_profile_events = 0
    grapher_hdf5_write_profile_file_size_bytes = 0
    grapher_hdf5_write_profile_write_us = 0.0
    grapher_hdf5_subphase_profile_count = 0
    grapher_hdf5_subphase_lock_wait_us = 0.0
    grapher_hdf5_subphase_prep_us = 0.0
    grapher_hdf5_subphase_prep_scan_us = 0.0
    grapher_hdf5_subphase_prep_build_us = 0.0
    grapher_hdf5_subphase_prep_payload_copy_us = 0.0
    grapher_hdf5_subphase_filename_scan_us = 0.0
    grapher_hdf5_subphase_open_us = 0.0
    grapher_hdf5_subphase_dataset_write_us = 0.0
    grapher_hdf5_subphase_dataset_group_create_us = 0.0
    grapher_hdf5_subphase_dataset_dataspace_create_us = 0.0
    grapher_hdf5_subphase_dataset_datatype_create_us = 0.0
    grapher_hdf5_subphase_dataset_create_us = 0.0
    grapher_hdf5_subphase_dataset_write_call_us = 0.0
    grapher_hdf5_subphase_dataset_payload_write_call_us = 0.0
    grapher_hdf5_subphase_dataset_meta_write_call_us = 0.0
    grapher_hdf5_subphase_dataset_object_close_us = 0.0
    grapher_hdf5_subphase_raw_payload_open_us = 0.0
    grapher_hdf5_subphase_raw_payload_preallocate_us = 0.0
    grapher_hdf5_subphase_raw_payload_writev_us = 0.0
    grapher_hdf5_subphase_raw_payload_close_us = 0.0
    grapher_hdf5_subphase_raw_payload_close_wait_us = 0.0
    grapher_hdf5_subphase_raw_payload_write_wait_us = 0.0
    grapher_hdf5_subphase_raw_payload_bytes = 0
    grapher_hdf5_subphase_raw_payload_writev_calls = 0
    grapher_hdf5_subphase_raw_payload_partial_writes = 0
    grapher_hdf5_subphase_raw_payload_preallocate_result = ""
    grapher_hdf5_subphase_flush_us = 0.0
    grapher_hdf5_subphase_file_size_us = 0.0
    grapher_hdf5_subphase_close_us = 0.0
    grapher_hdf5_subphase_rename_us = 0.0
    grapher_hdf5_subphase_publish_rename_us = 0.0
    grapher_hdf5_subphase_archive_manifest_write_us = 0.0
    grapher_hdf5_subphase_writer_total_us = 0.0
    grapher_hdf5_subphase_atomic_rename = ""
    grapher_hdf5_subphase_chunk_events = ""
    grapher_hdf5_subphase_layout = ""
    grapher_async_archive_publish_count = 0
    grapher_async_archive_publish_success_count = 0
    grapher_async_archive_publish_close_wait_us = 0.0
    grapher_async_archive_publish_close_us = 0.0
    grapher_async_archive_publish_rename_us = 0.0
    grapher_async_archive_publish_manifest_us = 0.0
    grapher_async_archive_publish_total_us = 0.0
    orphan_first = None
    orphan_last = None
    orphan_count = 0
    orphan_discarded_count = 0
    if story_id or aggregate_all_stories:
        drain_pattern = re.compile(
            rf"Successfully drained a story chunk to Grapher, StoryID: {story_pattern}, StartTime:"
        )
        keeper_flush_start_pattern = re.compile(
            rf"Flush story recording requested\. StoryID={story_pattern} "
        )
        keeper_flush_complete_pattern = re.compile(
            rf"Flush story recording completed\. StoryID={story_pattern} "
            r"drained=(?P<drained>[01]) queuedChunks=(?P<queued>\d+) inFlightChunks=(?P<inflight>\d+)"
            r"(?: finalize_us=(?P<finalize>[0-9.]+) drain_wait_us=(?P<wait>[0-9.]+) total_us=(?P<total>[0-9.]+))?"
        )
        keeper_drain_profile_pattern = re.compile(
            rf"Drain profile StoryID={story_pattern} StartTime=\d+ "
            r"(?:EndTime=\d+ receiver=\S+ )?"
            r"eventCount=(?P<events>\d+) serializedBytes=(?P<bytes>\d+) serialization_us=(?P<serialization>[0-9.]+) "
            r"drain_rpc_us=(?P<rpc>[0-9.]+) total_us=(?P<total>[0-9.]+)"
        )
        rdma_transfer_profile_pattern = re.compile(
            r"Transfer profile (?:transferId=\d+ receiver=\S+ )?"
            r"bytes=(?P<bytes>\d+) tlBulkBytes=(?P<bulk>\d+) "
            r"transferredBytes=(?P<transferred>\d+) expose_us=(?P<expose>[0-9.]+) "
            r"remote_call_us=(?P<call>[0-9.]+) total_us=(?P<total>[0-9.]+)"
        )
        keeper_log_re = re.compile(r"^chrono-keeper-(?P<host>.+?)(?:\.\d+)?\.log")
        orphan_pattern = re.compile(rf"Orphan chunk for story {story_pattern}\.")
        orphan_discard_pattern = re.compile(r"Discarded (?P<count>\d+) orphan chunks\.")
        grapher_receive_profile_pattern = re.compile(
            rf"Receive profile StoryID={story_pattern} StartTime=\d+ "
            r"(?:EndTime=\d+ )?"
            r"eventCount=(?P<events>\d+) "
            r"bytes=(?P<bytes>\d+) "
            r"(?:(?:allocation_us=(?P<allocation>[0-9.]+) "
            r"local_bulk_expose_us=(?P<local_expose>[0-9.]+) )?)"
            r"bulk_transfer_us=(?P<bulk>[0-9.]+) "
            r"deserialization_us=(?P<deserialization>[0-9.]+) ingestion_us=(?P<ingestion>[0-9.]+) "
            r"(?:(?:response_us=(?P<response>[0-9.]+) total_before_response_us=(?P<before>[0-9.]+) "
            r"total_with_response_us=(?P<with>[0-9.]+))|(?:total_us=(?P<legacy_total>[0-9.]+)))"
        )
        grapher_archive_drain_pattern = re.compile(
            rf"Stop story archive drain completed\. StoryId={story_pattern} "
            r"drainEnabled=(?P<enabled>[01]) drained=(?P<drained>[01]) "
            r"queuedChunks=(?P<queued>\d+) inFlightChunks=(?P<inflight>\d+) "
            r"finalize_us=(?P<finalize>[0-9.]+) drain_wait_us=(?P<wait>[0-9.]+)"
        )
        grapher_stop_retire_profile_pattern = re.compile(
            rf"\[GrapherStopRetireProfile\] story_id={story_pattern} async=(?P<async>[01]) "
            r"initial_lock_wait_us=(?P<initial_lock_wait>[0-9.]+) "
            r"initial_lock_hold_us=(?P<initial_lock_hold>[0-9.]+) "
            r"completion_wait_us=(?P<completion_wait>[0-9.]+) "
            r"collect_erase_us=(?P<collect_erase>[0-9.]+) "
            r"async_lock_wait_us=(?P<async_lock_wait>[0-9.]+) "
            r"finalize_us=(?P<finalize>[0-9.]+) "
            r"archive_drain_wait_us=(?P<archive_drain_wait>[0-9.]+) "
            r"total_us=(?P<total>[0-9.]+)"
        )
        grapher_story_pipeline_merge_profile_pattern = re.compile(
            rf"Merge profile StoryID={story_pattern} sourceStartTime=\d+ sourceEndTime=\d+ "
            r"sourceEventCount=(?P<source>\d+) sourceFirstEventTime=\d+ "
            r"mergedEventCount=(?P<merged>\d+) remainingEventCount=(?P<remaining>\d+) "
            r"total_us=(?P<total>[0-9.]+)"
        )
        grapher_hdf5_write_profile_pattern = re.compile(
            rf"Write profile StoryID={story_pattern} StartTime=\d+ eventCount=(?P<events>\d+) "
            r"fileSize=(?P<size>\d+) write_us=(?P<write>[0-9.]+)"
        )
        grapher_hdf5_subphase_profile_pattern = re.compile(
            rf"Write subphase profile StoryID={story_pattern} StartTime=\d+ eventCount=(?P<events>\d+) "
            r"fileSize=(?P<size>\d+) (?:layout=(?P<layout>[A-Za-z0-9_]+) )?(?:atomic_rename=(?P<atomic>[01]) )?"
            r"(?:chunk_events=(?P<chunk>\d+) )?(?:prep_us=(?P<prep>[0-9.]+) )?"
            r"(?:(?:prep_scan_us=(?P<prep_scan>[0-9.]+) "
            r"prep_build_us=(?P<prep_build>[0-9.]+) "
            r"prep_payload_copy_us=(?P<prep_payload_copy>[0-9.]+) )?)"
            r"lock_wait_us=(?P<lock>[0-9.]+) "
            r"filename_scan_us=(?P<filename>[0-9.]+) open_us=(?P<open>[0-9.]+) "
            r"dataset_write_us=(?P<dataset>[0-9.]+) flush_us=(?P<flush>[0-9.]+) "
            r"(?:(?:dataset_group_create_us=(?P<dataset_group>[0-9.]+) "
            r"dataset_dataspace_create_us=(?P<dataset_dataspace>[0-9.]+) "
            r"dataset_datatype_create_us=(?P<dataset_datatype>[0-9.]+) "
            r"dataset_create_us=(?P<dataset_create>[0-9.]+) "
            r"dataset_write_call_us=(?P<dataset_write_call>[0-9.]+) "
            r"(?:dataset_payload_write_call_us=(?P<dataset_payload_write_call>[0-9.]+) )?"
            r"(?:dataset_meta_write_call_us=(?P<dataset_meta_write_call>[0-9.]+) )?"
            r"dataset_object_close_us=(?P<dataset_object_close>[0-9.]+) "
            r"(?:(?:raw_payload_open_us=(?P<raw_payload_open>[0-9.]+) "
            r"(?:raw_payload_preallocate_us=(?P<raw_payload_preallocate>[0-9.]+) )?"
            r"raw_payload_writev_us=(?P<raw_payload_writev>[0-9.]+) "
            r"raw_payload_close_us=(?P<raw_payload_close>[0-9.]+) "
            r"(?:raw_payload_close_wait_us=(?P<raw_payload_close_wait>[0-9.]+) )?"
            r"(?:raw_payload_write_wait_us=(?P<raw_payload_write_wait>[0-9.]+) )?"
            r"raw_payload_bytes=(?P<raw_payload_bytes>\d+) "
            r"raw_payload_writev_calls=(?P<raw_payload_writev_calls>\d+) "
            r"raw_payload_partial_writes=(?P<raw_payload_partial_writes>\d+) "
            r"(?:raw_payload_preallocate_result=(?P<raw_payload_preallocate_result>-?\d+) )?)?)"
            r")?)"
            r"(?:raw_payload_async_write=(?P<raw_payload_async_write>[01]) )?"
            r"(?:raw_payload_async_close=(?P<raw_payload_async_close>[01]) )?"
            r"(?:raw_payload_async_publish=(?P<raw_payload_async_publish>[01]) )?"
            r"(?:raw_payload_async_publish_threads=(?P<raw_payload_async_publish_threads>\d+) )?"
            r"file_size_us=(?P<filesize>[0-9.]+) (?:close_us=(?P<close>[0-9.]+) )?"
            r"(?:rename_us=(?P<rename>[0-9.]+) )?"
            r"(?:publish_rename_us=(?P<publish_rename>[0-9.]+) )?"
            r"(?:archive_manifest_write_us=(?P<archive_manifest_write>[0-9.]+) )?"
            r"writer_total_us=(?P<writer_total>[0-9.]+) "
            r"write_us=(?P<write>[0-9.]+)"
        )
        grapher_async_archive_publish_pattern = re.compile(
            rf"Async archive publish completed StoryID={story_pattern} StartTime=\d+ "
            r"(?:worker=\d+ )?ok=(?P<ok>[01]) "
            r"close_wait_us=(?P<close_wait>[0-9.]+) close_us=(?P<close>[0-9.]+) "
            r"publish_rename_us=(?P<rename>[0-9.]+) archive_manifest_write_us=(?P<manifest>[0-9.]+) "
            r"total_us=(?P<total>[0-9.]+)"
        )
        for path in sorted(log_dir.glob("chrono-keeper-*.log*")):
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            host_match = keeper_log_re.match(path.name)
            host = host_match.group("host") if host_match else path.name
            for line in lines:
                timestamp = parse_log_timestamp(line)
                if timestamp is None:
                    continue
                if drain_pattern.search(line):
                    drain_count += 1
                    drain_first = timestamp if drain_first is None else drain_first
                    drain_last = timestamp
                if keeper_flush_start_pattern.search(line):
                    keeper_flush_starts[host] = timestamp
                    keeper_flush_start_first = (
                        timestamp if keeper_flush_start_first is None else min(keeper_flush_start_first, timestamp)
                    )
                if keeper_flush_complete_pattern.search(line):
                    complete_match = keeper_flush_complete_pattern.search(line)
                    keeper_flush_complete_last = (
                        timestamp
                        if keeper_flush_complete_last is None
                        else max(keeper_flush_complete_last, timestamp)
                    )
                    if host in keeper_flush_starts:
                        keeper_flush_durations[host] = timestamp - keeper_flush_starts[host]
                    if complete_match:
                        if complete_match.group("finalize") is not None:
                            keeper_flush_finalize_us.append(float(complete_match.group("finalize")))
                        if complete_match.group("wait") is not None:
                            keeper_flush_wait_us.append(float(complete_match.group("wait")))
                        if complete_match.group("total") is not None:
                            keeper_flush_total_us.append(float(complete_match.group("total")))
                drain_profile_match = keeper_drain_profile_pattern.search(line)
                if drain_profile_match:
                    keeper_drain_profile_count += 1
                    keeper_drain_profile_events += int(drain_profile_match.group("events"))
                    keeper_drain_profile_bytes += int(drain_profile_match.group("bytes"))
                    keeper_drain_profile_serialization_us.append(float(drain_profile_match.group("serialization")))
                    keeper_drain_profile_rpc_us.append(float(drain_profile_match.group("rpc")))
                    keeper_drain_profile_total_us.append(float(drain_profile_match.group("total")))
                transfer_profile_match = rdma_transfer_profile_pattern.search(line)
                if transfer_profile_match:
                    rdma_transfer_profile_count += 1
                    rdma_transfer_profile_bytes += int(transfer_profile_match.group("bytes"))
                    rdma_transfer_profile_transferred_bytes += int(transfer_profile_match.group("transferred"))
                    rdma_transfer_profile_expose_us += float(transfer_profile_match.group("expose"))
                    rdma_transfer_profile_remote_call_us += float(transfer_profile_match.group("call"))
                    rdma_transfer_profile_total_us += float(transfer_profile_match.group("total"))
        for path in sorted(log_dir.glob("chrono-grapher-*.log*")):
            try:
                lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            except OSError:
                continue
            for line in lines:
                timestamp = parse_log_timestamp(line)
                if orphan_pattern.search(line):
                    if timestamp is None:
                        continue
                    orphan_count += 1
                    orphan_first = timestamp if orphan_first is None else orphan_first
                    orphan_last = timestamp
                orphan_discard_match = orphan_discard_pattern.search(line)
                if orphan_discard_match:
                    orphan_discarded_count += int(orphan_discard_match.group("count"))
                receive_profile_match = grapher_receive_profile_pattern.search(line)
                if receive_profile_match:
                    grapher_receive_profile_count += 1
                    grapher_receive_profile_events += int(receive_profile_match.group("events"))
                    grapher_receive_profile_bytes += int(receive_profile_match.group("bytes"))
                    if receive_profile_match.group("allocation") is not None:
                        grapher_receive_profile_allocation_us.append(float(receive_profile_match.group("allocation")))
                    if receive_profile_match.group("local_expose") is not None:
                        grapher_receive_profile_local_bulk_expose_us.append(
                            float(receive_profile_match.group("local_expose"))
                        )
                    grapher_receive_profile_bulk_transfer_us.append(float(receive_profile_match.group("bulk")))
                    grapher_receive_profile_deserialization_us.append(
                        float(receive_profile_match.group("deserialization"))
                    )
                    grapher_receive_profile_ingestion_us.append(float(receive_profile_match.group("ingestion")))
                    if receive_profile_match.group("response") is not None:
                        grapher_receive_profile_response_us.append(float(receive_profile_match.group("response")))
                        grapher_receive_profile_total_before_response_us.append(float(receive_profile_match.group("before")))
                        grapher_receive_profile_total_with_response_us.append(float(receive_profile_match.group("with")))
                    elif receive_profile_match.group("legacy_total") is not None:
                        legacy_total_us = float(receive_profile_match.group("legacy_total"))
                        grapher_receive_profile_total_before_response_us.append(legacy_total_us)
                        grapher_receive_profile_total_with_response_us.append(legacy_total_us)
                archive_drain_match = grapher_archive_drain_pattern.search(line)
                if archive_drain_match:
                    grapher_archive_drain_count += 1
                    grapher_archive_drain_finalize_us.append(float(archive_drain_match.group("finalize")))
                    grapher_archive_drain_wait_us.append(float(archive_drain_match.group("wait")))
                    if int(archive_drain_match.group("queued")) > 0:
                        grapher_archive_drain_queued_nonzero_count += 1
                    if int(archive_drain_match.group("inflight")) > 0:
                        grapher_archive_drain_inflight_nonzero_count += 1
                stop_retire_profile_match = grapher_stop_retire_profile_pattern.search(line)
                if stop_retire_profile_match:
                    grapher_stop_retire_profile_count += 1
                    if stop_retire_profile_match.group("async") == "1":
                        grapher_stop_retire_profile_async_count += 1
                    grapher_stop_retire_initial_lock_wait_us.append(
                        float(stop_retire_profile_match.group("initial_lock_wait"))
                    )
                    grapher_stop_retire_initial_lock_hold_us.append(
                        float(stop_retire_profile_match.group("initial_lock_hold"))
                    )
                    grapher_stop_retire_completion_wait_us.append(
                        float(stop_retire_profile_match.group("completion_wait"))
                    )
                    grapher_stop_retire_collect_erase_us.append(
                        float(stop_retire_profile_match.group("collect_erase"))
                    )
                    grapher_stop_retire_async_lock_wait_us.append(
                        float(stop_retire_profile_match.group("async_lock_wait"))
                    )
                    grapher_stop_retire_finalize_us.append(float(stop_retire_profile_match.group("finalize")))
                    grapher_stop_retire_archive_drain_wait_us.append(
                        float(stop_retire_profile_match.group("archive_drain_wait"))
                    )
                    grapher_stop_retire_total_us.append(float(stop_retire_profile_match.group("total")))
                merge_profile_match = grapher_story_pipeline_merge_profile_pattern.search(line)
                if merge_profile_match:
                    grapher_story_pipeline_merge_profile_count += 1
                    grapher_story_pipeline_merge_profile_source_events += int(merge_profile_match.group("source"))
                    grapher_story_pipeline_merge_profile_merged_events += int(merge_profile_match.group("merged"))
                    grapher_story_pipeline_merge_profile_remaining_events += int(
                        merge_profile_match.group("remaining")
                    )
                    grapher_story_pipeline_merge_profile_total_us += float(merge_profile_match.group("total"))
                hdf5_profile_match = grapher_hdf5_write_profile_pattern.search(line)
                if hdf5_profile_match:
                    grapher_hdf5_write_profile_count += 1
                    grapher_hdf5_write_profile_events += int(hdf5_profile_match.group("events"))
                    grapher_hdf5_write_profile_file_size_bytes += int(hdf5_profile_match.group("size"))
                    grapher_hdf5_write_profile_write_us += float(hdf5_profile_match.group("write"))
                hdf5_subphase_match = grapher_hdf5_subphase_profile_pattern.search(line)
                if hdf5_subphase_match:
                    grapher_hdf5_subphase_profile_count += 1
                    if hdf5_subphase_match.group("layout") is not None:
                        grapher_hdf5_subphase_layout = hdf5_subphase_match.group("layout")
                    if hdf5_subphase_match.group("atomic") is not None:
                        grapher_hdf5_subphase_atomic_rename = hdf5_subphase_match.group("atomic")
                    if hdf5_subphase_match.group("chunk") is not None:
                        grapher_hdf5_subphase_chunk_events = hdf5_subphase_match.group("chunk")
                    if hdf5_subphase_match.group("prep") is not None:
                        grapher_hdf5_subphase_prep_us += float(hdf5_subphase_match.group("prep"))
                    if hdf5_subphase_match.group("prep_scan") is not None:
                        grapher_hdf5_subphase_prep_scan_us += float(hdf5_subphase_match.group("prep_scan"))
                    if hdf5_subphase_match.group("prep_build") is not None:
                        grapher_hdf5_subphase_prep_build_us += float(hdf5_subphase_match.group("prep_build"))
                    if hdf5_subphase_match.group("prep_payload_copy") is not None:
                        grapher_hdf5_subphase_prep_payload_copy_us += float(
                            hdf5_subphase_match.group("prep_payload_copy")
                        )
                    grapher_hdf5_subphase_lock_wait_us += float(hdf5_subphase_match.group("lock"))
                    grapher_hdf5_subphase_filename_scan_us += float(hdf5_subphase_match.group("filename"))
                    grapher_hdf5_subphase_open_us += float(hdf5_subphase_match.group("open"))
                    grapher_hdf5_subphase_dataset_write_us += float(hdf5_subphase_match.group("dataset"))
                    if hdf5_subphase_match.group("dataset_group") is not None:
                        grapher_hdf5_subphase_dataset_group_create_us += float(
                            hdf5_subphase_match.group("dataset_group")
                        )
                    if hdf5_subphase_match.group("dataset_dataspace") is not None:
                        grapher_hdf5_subphase_dataset_dataspace_create_us += float(
                            hdf5_subphase_match.group("dataset_dataspace")
                        )
                    if hdf5_subphase_match.group("dataset_datatype") is not None:
                        grapher_hdf5_subphase_dataset_datatype_create_us += float(
                            hdf5_subphase_match.group("dataset_datatype")
                        )
                    if hdf5_subphase_match.group("dataset_create") is not None:
                        grapher_hdf5_subphase_dataset_create_us += float(
                            hdf5_subphase_match.group("dataset_create")
                        )
                    if hdf5_subphase_match.group("dataset_write_call") is not None:
                        grapher_hdf5_subphase_dataset_write_call_us += float(
                            hdf5_subphase_match.group("dataset_write_call")
                        )
                    if hdf5_subphase_match.group("dataset_payload_write_call") is not None:
                        grapher_hdf5_subphase_dataset_payload_write_call_us += float(
                            hdf5_subphase_match.group("dataset_payload_write_call")
                        )
                    if hdf5_subphase_match.group("dataset_meta_write_call") is not None:
                        grapher_hdf5_subphase_dataset_meta_write_call_us += float(
                            hdf5_subphase_match.group("dataset_meta_write_call")
                        )
                    if hdf5_subphase_match.group("dataset_object_close") is not None:
                        grapher_hdf5_subphase_dataset_object_close_us += float(
                            hdf5_subphase_match.group("dataset_object_close")
                        )
                    if hdf5_subphase_match.group("raw_payload_open") is not None:
                        grapher_hdf5_subphase_raw_payload_open_us += float(
                            hdf5_subphase_match.group("raw_payload_open")
                        )
                    if hdf5_subphase_match.group("raw_payload_preallocate") is not None:
                        grapher_hdf5_subphase_raw_payload_preallocate_us += float(
                            hdf5_subphase_match.group("raw_payload_preallocate")
                        )
                    if hdf5_subphase_match.group("raw_payload_writev") is not None:
                        grapher_hdf5_subphase_raw_payload_writev_us += float(
                            hdf5_subphase_match.group("raw_payload_writev")
                        )
                    if hdf5_subphase_match.group("raw_payload_close") is not None:
                        grapher_hdf5_subphase_raw_payload_close_us += float(
                            hdf5_subphase_match.group("raw_payload_close")
                        )
                    if hdf5_subphase_match.group("raw_payload_close_wait") is not None:
                        grapher_hdf5_subphase_raw_payload_close_wait_us += float(
                            hdf5_subphase_match.group("raw_payload_close_wait")
                        )
                    if hdf5_subphase_match.group("raw_payload_write_wait") is not None:
                        grapher_hdf5_subphase_raw_payload_write_wait_us += float(
                            hdf5_subphase_match.group("raw_payload_write_wait")
                        )
                    if hdf5_subphase_match.group("raw_payload_bytes") is not None:
                        grapher_hdf5_subphase_raw_payload_bytes += int(
                            hdf5_subphase_match.group("raw_payload_bytes")
                        )
                    if hdf5_subphase_match.group("raw_payload_writev_calls") is not None:
                        grapher_hdf5_subphase_raw_payload_writev_calls += int(
                            hdf5_subphase_match.group("raw_payload_writev_calls")
                        )
                    if hdf5_subphase_match.group("raw_payload_partial_writes") is not None:
                        grapher_hdf5_subphase_raw_payload_partial_writes += int(
                            hdf5_subphase_match.group("raw_payload_partial_writes")
                        )
                    if hdf5_subphase_match.group("raw_payload_preallocate_result") is not None:
                        grapher_hdf5_subphase_raw_payload_preallocate_result = hdf5_subphase_match.group(
                            "raw_payload_preallocate_result"
                        )
                    grapher_hdf5_subphase_flush_us += float(hdf5_subphase_match.group("flush"))
                    grapher_hdf5_subphase_file_size_us += float(hdf5_subphase_match.group("filesize"))
                    if hdf5_subphase_match.group("close") is not None:
                        grapher_hdf5_subphase_close_us += float(hdf5_subphase_match.group("close"))
                    if hdf5_subphase_match.group("rename") is not None:
                        grapher_hdf5_subphase_rename_us += float(hdf5_subphase_match.group("rename"))
                    if hdf5_subphase_match.group("publish_rename") is not None:
                        grapher_hdf5_subphase_publish_rename_us += float(
                            hdf5_subphase_match.group("publish_rename")
                        )
                    if hdf5_subphase_match.group("archive_manifest_write") is not None:
                        grapher_hdf5_subphase_archive_manifest_write_us += float(
                            hdf5_subphase_match.group("archive_manifest_write")
                        )
                    grapher_hdf5_subphase_writer_total_us += float(hdf5_subphase_match.group("writer_total"))
                async_publish_match = grapher_async_archive_publish_pattern.search(line)
                if async_publish_match:
                    grapher_async_archive_publish_count += 1
                    grapher_async_archive_publish_success_count += int(async_publish_match.group("ok"))
                    grapher_async_archive_publish_close_wait_us += float(async_publish_match.group("close_wait"))
                    grapher_async_archive_publish_close_us += float(async_publish_match.group("close"))
                    grapher_async_archive_publish_rename_us += float(async_publish_match.group("rename"))
                    grapher_async_archive_publish_manifest_us += float(async_publish_match.group("manifest"))
                    grapher_async_archive_publish_total_us += float(async_publish_match.group("total"))
    row["keeper_to_grapher_drain_chunk_count"] = drain_count
    row["keeper_to_grapher_drain_first_epoch_seconds"] = drain_first
    row["keeper_to_grapher_drain_last_epoch_seconds"] = drain_last
    row["release_to_keeper_to_grapher_drain_first_seconds"] = elapsed_since(release_returned_at, drain_first)
    row["release_to_keeper_to_grapher_drain_last_seconds"] = elapsed_since(release_returned_at, drain_last)
    row["keeper_flush_story_start_first_epoch_seconds"] = keeper_flush_start_first
    row["keeper_flush_story_complete_last_epoch_seconds"] = keeper_flush_complete_last
    row["release_to_keeper_flush_story_start_first_seconds"] = elapsed_since(release_returned_at, keeper_flush_start_first)
    row["release_to_keeper_flush_story_complete_last_seconds"] = elapsed_since(
        release_returned_at, keeper_flush_complete_last
    )
    row["keeper_flush_story_max_seconds"] = max(keeper_flush_durations.values()) if keeper_flush_durations else None
    row["keeper_flush_story_count"] = len(keeper_flush_durations)
    row["keeper_flush_finalize_max_us"] = max(keeper_flush_finalize_us) if keeper_flush_finalize_us else None
    row["keeper_flush_drain_wait_max_us"] = max(keeper_flush_wait_us) if keeper_flush_wait_us else None
    row["keeper_flush_total_max_us"] = max(keeper_flush_total_us) if keeper_flush_total_us else None
    row["keeper_drain_profile_count"] = keeper_drain_profile_count
    row["keeper_drain_profile_events"] = keeper_drain_profile_events
    row["keeper_drain_profile_bytes"] = keeper_drain_profile_bytes
    add_microsecond_sample_stats(row, "keeper_drain_profile_serialization", keeper_drain_profile_serialization_us)
    add_microsecond_sample_stats(row, "keeper_drain_profile_rpc", keeper_drain_profile_rpc_us)
    add_microsecond_sample_stats(row, "keeper_drain_profile_total", keeper_drain_profile_total_us)
    row["rdma_transfer_profile_count"] = rdma_transfer_profile_count
    row["rdma_transfer_profile_bytes"] = rdma_transfer_profile_bytes
    row["rdma_transfer_profile_transferred_bytes"] = rdma_transfer_profile_transferred_bytes
    row["rdma_transfer_profile_expose_us"] = rdma_transfer_profile_expose_us
    row["rdma_transfer_profile_remote_call_us"] = rdma_transfer_profile_remote_call_us
    row["rdma_transfer_profile_total_us"] = rdma_transfer_profile_total_us
    row["grapher_receive_profile_count"] = grapher_receive_profile_count
    row["grapher_receive_profile_events"] = grapher_receive_profile_events
    row["grapher_receive_profile_bytes"] = grapher_receive_profile_bytes
    add_microsecond_sample_stats(row, "grapher_receive_profile_allocation", grapher_receive_profile_allocation_us)
    add_microsecond_sample_stats(
        row, "grapher_receive_profile_local_bulk_expose", grapher_receive_profile_local_bulk_expose_us
    )
    add_microsecond_sample_stats(row, "grapher_receive_profile_bulk_transfer", grapher_receive_profile_bulk_transfer_us)
    add_microsecond_sample_stats(row, "grapher_receive_profile_deserialization", grapher_receive_profile_deserialization_us)
    add_microsecond_sample_stats(row, "grapher_receive_profile_ingestion", grapher_receive_profile_ingestion_us)
    add_microsecond_sample_stats(row, "grapher_receive_profile_response", grapher_receive_profile_response_us)
    add_microsecond_sample_stats(
        row, "grapher_receive_profile_total_before_response", grapher_receive_profile_total_before_response_us
    )
    add_microsecond_sample_stats(
        row, "grapher_receive_profile_total_with_response", grapher_receive_profile_total_with_response_us
    )
    row["grapher_archive_drain_count"] = grapher_archive_drain_count
    row["grapher_archive_drain_finalize_max_us"] = (
        max(grapher_archive_drain_finalize_us) if grapher_archive_drain_finalize_us else None
    )
    row["grapher_archive_drain_wait_max_us"] = (
        max(grapher_archive_drain_wait_us) if grapher_archive_drain_wait_us else None
    )
    row["grapher_archive_drain_wait_total_us"] = sum(grapher_archive_drain_wait_us)
    row["grapher_archive_drain_queued_nonzero_count"] = grapher_archive_drain_queued_nonzero_count
    row["grapher_archive_drain_inflight_nonzero_count"] = grapher_archive_drain_inflight_nonzero_count
    row["grapher_stop_retire_profile_count"] = grapher_stop_retire_profile_count
    row["grapher_stop_retire_profile_async_count"] = grapher_stop_retire_profile_async_count
    row["grapher_stop_retire_initial_lock_wait_max_us"] = (
        max(grapher_stop_retire_initial_lock_wait_us) if grapher_stop_retire_initial_lock_wait_us else None
    )
    row["grapher_stop_retire_initial_lock_hold_max_us"] = (
        max(grapher_stop_retire_initial_lock_hold_us) if grapher_stop_retire_initial_lock_hold_us else None
    )
    row["grapher_stop_retire_completion_wait_max_us"] = (
        max(grapher_stop_retire_completion_wait_us) if grapher_stop_retire_completion_wait_us else None
    )
    row["grapher_stop_retire_collect_erase_max_us"] = (
        max(grapher_stop_retire_collect_erase_us) if grapher_stop_retire_collect_erase_us else None
    )
    row["grapher_stop_retire_async_lock_wait_max_us"] = (
        max(grapher_stop_retire_async_lock_wait_us) if grapher_stop_retire_async_lock_wait_us else None
    )
    row["grapher_stop_retire_finalize_max_us"] = (
        max(grapher_stop_retire_finalize_us) if grapher_stop_retire_finalize_us else None
    )
    row["grapher_stop_retire_archive_drain_wait_max_us"] = (
        max(grapher_stop_retire_archive_drain_wait_us) if grapher_stop_retire_archive_drain_wait_us else None
    )
    row["grapher_stop_retire_total_max_us"] = max(grapher_stop_retire_total_us) if grapher_stop_retire_total_us else None
    row["grapher_story_pipeline_merge_profile_count"] = grapher_story_pipeline_merge_profile_count
    row["grapher_story_pipeline_merge_profile_source_events"] = grapher_story_pipeline_merge_profile_source_events
    row["grapher_story_pipeline_merge_profile_merged_events"] = grapher_story_pipeline_merge_profile_merged_events
    row["grapher_story_pipeline_merge_profile_remaining_events"] = grapher_story_pipeline_merge_profile_remaining_events
    row["grapher_story_pipeline_merge_profile_total_us"] = grapher_story_pipeline_merge_profile_total_us
    row["grapher_hdf5_write_profile_count"] = grapher_hdf5_write_profile_count
    row["grapher_hdf5_write_profile_events"] = grapher_hdf5_write_profile_events
    row["grapher_hdf5_write_profile_file_size_bytes"] = grapher_hdf5_write_profile_file_size_bytes
    row["grapher_hdf5_write_profile_write_us"] = grapher_hdf5_write_profile_write_us
    row["grapher_hdf5_subphase_profile_count"] = grapher_hdf5_subphase_profile_count
    row["grapher_hdf5_subphase_prep_us"] = grapher_hdf5_subphase_prep_us
    row["grapher_hdf5_subphase_prep_scan_us"] = grapher_hdf5_subphase_prep_scan_us
    row["grapher_hdf5_subphase_prep_build_us"] = grapher_hdf5_subphase_prep_build_us
    row["grapher_hdf5_subphase_prep_payload_copy_us"] = grapher_hdf5_subphase_prep_payload_copy_us
    row["grapher_hdf5_subphase_lock_wait_us"] = grapher_hdf5_subphase_lock_wait_us
    row["grapher_hdf5_subphase_filename_scan_us"] = grapher_hdf5_subphase_filename_scan_us
    row["grapher_hdf5_subphase_open_us"] = grapher_hdf5_subphase_open_us
    row["grapher_hdf5_subphase_dataset_write_us"] = grapher_hdf5_subphase_dataset_write_us
    row["grapher_hdf5_subphase_dataset_group_create_us"] = grapher_hdf5_subphase_dataset_group_create_us
    row["grapher_hdf5_subphase_dataset_dataspace_create_us"] = grapher_hdf5_subphase_dataset_dataspace_create_us
    row["grapher_hdf5_subphase_dataset_datatype_create_us"] = grapher_hdf5_subphase_dataset_datatype_create_us
    row["grapher_hdf5_subphase_dataset_create_us"] = grapher_hdf5_subphase_dataset_create_us
    row["grapher_hdf5_subphase_dataset_write_call_us"] = grapher_hdf5_subphase_dataset_write_call_us
    row["grapher_hdf5_subphase_dataset_payload_write_call_us"] = grapher_hdf5_subphase_dataset_payload_write_call_us
    row["grapher_hdf5_subphase_dataset_meta_write_call_us"] = grapher_hdf5_subphase_dataset_meta_write_call_us
    row["grapher_hdf5_subphase_dataset_object_close_us"] = grapher_hdf5_subphase_dataset_object_close_us
    row["grapher_hdf5_subphase_raw_payload_open_us"] = grapher_hdf5_subphase_raw_payload_open_us
    row["grapher_hdf5_subphase_raw_payload_preallocate_us"] = grapher_hdf5_subphase_raw_payload_preallocate_us
    row["grapher_hdf5_subphase_raw_payload_writev_us"] = grapher_hdf5_subphase_raw_payload_writev_us
    row["grapher_hdf5_subphase_raw_payload_close_us"] = grapher_hdf5_subphase_raw_payload_close_us
    row["grapher_hdf5_subphase_raw_payload_close_wait_us"] = grapher_hdf5_subphase_raw_payload_close_wait_us
    row["grapher_hdf5_subphase_raw_payload_write_wait_us"] = grapher_hdf5_subphase_raw_payload_write_wait_us
    row["grapher_hdf5_subphase_raw_payload_bytes"] = grapher_hdf5_subphase_raw_payload_bytes
    row["grapher_hdf5_subphase_raw_payload_writev_calls"] = grapher_hdf5_subphase_raw_payload_writev_calls
    row["grapher_hdf5_subphase_raw_payload_partial_writes"] = grapher_hdf5_subphase_raw_payload_partial_writes
    row["grapher_hdf5_subphase_raw_payload_preallocate_result"] = (
        grapher_hdf5_subphase_raw_payload_preallocate_result
    )
    row["grapher_hdf5_subphase_flush_us"] = grapher_hdf5_subphase_flush_us
    row["grapher_hdf5_subphase_file_size_us"] = grapher_hdf5_subphase_file_size_us
    row["grapher_hdf5_subphase_close_us"] = grapher_hdf5_subphase_close_us
    row["grapher_hdf5_subphase_rename_us"] = grapher_hdf5_subphase_rename_us
    row["grapher_hdf5_subphase_publish_rename_us"] = grapher_hdf5_subphase_publish_rename_us
    row["grapher_hdf5_subphase_archive_manifest_write_us"] = grapher_hdf5_subphase_archive_manifest_write_us
    row["grapher_hdf5_subphase_writer_total_us"] = grapher_hdf5_subphase_writer_total_us
    row["grapher_hdf5_subphase_atomic_rename"] = grapher_hdf5_subphase_atomic_rename
    row["grapher_hdf5_subphase_chunk_events"] = grapher_hdf5_subphase_chunk_events
    row["grapher_hdf5_subphase_layout"] = grapher_hdf5_subphase_layout
    row["grapher_async_archive_publish_count"] = grapher_async_archive_publish_count
    row["grapher_async_archive_publish_success_count"] = grapher_async_archive_publish_success_count
    row["grapher_async_archive_publish_close_wait_us"] = grapher_async_archive_publish_close_wait_us
    row["grapher_async_archive_publish_close_us"] = grapher_async_archive_publish_close_us
    row["grapher_async_archive_publish_rename_us"] = grapher_async_archive_publish_rename_us
    row["grapher_async_archive_publish_manifest_us"] = grapher_async_archive_publish_manifest_us
    row["grapher_async_archive_publish_total_us"] = grapher_async_archive_publish_total_us
    row["grapher_orphan_chunk_count"] = orphan_count
    row["grapher_orphan_discarded_chunk_count"] = orphan_discarded_count
    row["grapher_orphan_first_epoch_seconds"] = orphan_first
    row["grapher_orphan_last_epoch_seconds"] = orphan_last
    row["release_to_grapher_orphan_first_seconds"] = elapsed_since(release_returned_at, orphan_first)
    row["release_to_grapher_orphan_last_seconds"] = elapsed_since(release_returned_at, orphan_last)
    log_metric_prefixes = (
        "keeper_to_grapher_",
        "release_to_keeper_",
        "keeper_flush_",
        "keeper_drain_profile_",
        "rdma_transfer_profile_",
        "grapher_receive_profile_",
        "grapher_story_pipeline_merge_profile_",
        "grapher_hdf5_write_profile_",
        "grapher_hdf5_subphase_",
        "grapher_orphan_",
        "release_to_grapher_orphan_",
        "grapher_finalize_to_hdf5_",
    )
    try:
        metrics = json.loads(metrics_file.read_text(encoding="utf-8"))
        for key, value in row.items():
            if key == "story_id" or key.startswith(log_metric_prefixes):
                metrics[key] = value
        metrics_file.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    except OSError:
        pass


def refresh_keeper_append_completeness_from_logs(row: dict[str, Any], metrics_file: Path) -> None:
    if row.get("system") != "chronolog":
        return
    log_dir = metrics_file.parent / "logs"
    if not log_dir.exists():
        return

    stats_re = re.compile(r"\[KeeperAppendStats\]\s+(.*)")
    kv_re = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
    keeper_log_re = re.compile(r"^chrono-keeper-(?P<host>.+?)(?:\.\d+)?\.log$")
    per_keeper: dict[str, dict[str, Any]] = {}
    orphan_warning_count = 0
    keeper_hosts: set[str] = set()

    for log_path in sorted(log_dir.glob("chrono-keeper-*.log")):
        if ".launch." in log_path.name or ".restart." in log_path.name:
            continue
        match = keeper_log_re.match(log_path.name)
        if not match:
            continue
        host = match.group("host")
        keeper_hosts.add(host)
        try:
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            if "[IngestionQueue] Orphan event" in line:
                orphan_warning_count += 1
            stats_match = stats_re.search(line)
            if not stats_match:
                continue
            parsed: dict[str, Any] = {"log": str(log_path), "keeper_host": host}
            for key, value in kv_re.findall(stats_match.group(1)):
                if key == "context":
                    parsed[key] = value
                    continue
                try:
                    parsed[key] = float(value) if "." in value else int(value)
                except ValueError:
                    parsed[key] = value
            if "record_event_count" not in parsed:
                continue
            current = per_keeper.get(host)
            if current is None or int(parsed.get("record_event_count", 0)) >= int(
                current.get("record_event_count", 0)
            ):
                per_keeper[host] = parsed

    if not per_keeper:
        return

    expected = int(float(row.get("total_operation_count") or row.get("operation_count") or 0))
    observed = sum(int(item.get("record_event_count", 0)) for item in per_keeper.values())
    old_observed = int(float(row.get("keeper_record_event_count") or row.get("keeper_append_record_event_count") or 0))
    if observed < old_observed:
        return

    keeper_count_expected = len(keeper_hosts)
    keeper_count_with_stats = len(per_keeper)
    complete = (expected == 0 or observed == expected) and keeper_count_with_stats == keeper_count_expected
    if observed == old_observed and str(row.get("keeper_append_stats_complete")).strip().lower() in {"true", "1"}:
        return

    row["keeper_record_event_count"] = observed
    row["keeper_append_stats_complete"] = complete
    row["keeper_append_record_event_count"] = observed
    row["keeper_append_expected_record_event_count"] = expected
    row["keeper_append_record_event_count_matches_total"] = expected == 0 or observed == expected
    row["keeper_append_append_stats_complete"] = complete
    row["keeper_append_keeper_count_expected"] = keeper_count_expected
    row["keeper_append_keeper_count_with_append_stats"] = keeper_count_with_stats
    row["keeper_append_orphan_warning_count"] = orphan_warning_count
    try:
        metrics = json.loads(metrics_file.read_text(encoding="utf-8"))
        metrics["keeper_record_event_count"] = observed
        metrics["keeper_append_stats_complete"] = complete
        keeper_append_stats = metrics.setdefault("keeper_append_stats", {})
        if isinstance(keeper_append_stats, dict):
            summary = keeper_append_stats.setdefault("summary", {})
            if isinstance(summary, dict):
                summary["record_event_count"] = observed
                summary["expected_record_event_count"] = expected
                summary["record_event_count_matches_total"] = expected == 0 or observed == expected
                summary["append_stats_complete"] = complete
                summary["keeper_count_expected"] = keeper_count_expected
                summary["keeper_count_with_append_stats"] = keeper_count_with_stats
                summary["orphan_warning_count"] = orphan_warning_count
        metrics_file.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    except OSError:
        pass


def augment_chronolog_client_append_stats(row: dict[str, Any], metrics_file: Path) -> None:
    if row.get("system") != "chronolog":
        return
    result_dir = metrics_file.parent
    if not result_dir.exists():
        return

    stats_re = re.compile(r"\[ClientAppendStats\]\s+(.*)")
    kv_re = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
    per_pid: dict[str, dict[str, Any]] = {}
    for log_path in sorted(result_dir.glob("*.log")):
        try:
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            stats_match = stats_re.search(line)
            if not stats_match:
                continue
            parsed: dict[str, Any] = {"log": str(log_path)}
            for key, value in kv_re.findall(stats_match.group(1)):
                if key == "context":
                    parsed[key] = value
                    continue
                try:
                    parsed[key] = float(value) if "." in value else int(value)
                except ValueError:
                    parsed[key] = value
            pid = str(parsed.get("pid") or parsed.get("log"))
            current = per_pid.get(pid)
            if current is None or int(parsed.get("event_count", 0)) >= int(current.get("event_count", 0)):
                per_pid[pid] = parsed

    if not per_pid:
        return

    def total(key: str) -> float:
        return sum(float(item.get(key, 0) or 0) for item in per_pid.values())

    event_count = total("event_count")
    batch_count = total("batch_count")
    future_wait_count = total("future_wait_count")
    summary: dict[str, Any] = {
        "client_process_count_with_append_stats": len(per_pid),
        "client_append_batch_count": int(batch_count),
        "client_append_event_count": int(event_count),
        "client_append_payload_bytes": int(total("payload_bytes")),
        "client_append_future_count": int(total("future_count")),
        "client_append_future_wait_count": int(future_wait_count),
    }
    weighted_event_fields = ("event_build_avg_us", "keeper_select_avg_us")
    for key in weighted_event_fields:
        summary[f"client_append_{key}"] = (
            sum(float(item.get(key, 0) or 0) * float(item.get("event_count", 0) or 0) for item in per_pid.values())
            / event_count
            if event_count
            else 0.0
        )
    summary["client_append_rpc_submit_avg_us"] = (
        sum(float(item.get("rpc_submit_avg_us", 0) or 0) * float(item.get("batch_count", 0) or 0)
            for item in per_pid.values())
        / batch_count
        if batch_count
        else 0.0
    )
    summary["client_append_rpc_submit_max_us"] = max(
        float(item.get("rpc_submit_max_us", 0) or 0) for item in per_pid.values()
    )
    summary["client_append_future_wait_avg_us"] = (
        sum(float(item.get("future_wait_avg_us", 0) or 0) * float(item.get("future_wait_count", 0) or 0)
            for item in per_pid.values())
        / future_wait_count
        if future_wait_count
        else 0.0
    )
    summary["client_append_future_wait_max_us"] = max(
        float(item.get("future_wait_max_us", 0) or 0) for item in per_pid.values()
    )
    expected = int(float(row.get("total_message_count") or row.get("total_operation_count") or 0))
    summary["client_append_expected_event_count"] = expected
    summary["client_append_event_count_matches_total"] = expected == 0 or int(event_count) == expected

    row.update(summary)
    try:
        metrics = json.loads(metrics_file.read_text(encoding="utf-8"))
        metrics.update(summary)
        metrics["client_append_stats"] = {"summary": summary, "per_process": list(per_pid.values())}
        metrics_file.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    except OSError:
        pass


def augment_chronolog_control_path_profiles(row: dict[str, Any], metrics_file: Path) -> None:
    if row.get("system") != "chronolog" or row.get("workflow") not in {
        "append_durable",
        "archive_range_retrieval",
    }:
        return
    log_dir = metrics_file.parent / "logs"
    if not log_dir.exists():
        return
    summary = control_path_profile_metrics(log_dir)
    if not summary:
        return
    row.update(summary)
    try:
        metrics = json.loads(metrics_file.read_text(encoding="utf-8"))
        metrics.update(summary)
        metrics_file.write_text(json.dumps(metrics, indent=2) + "\n", encoding="utf-8")
    except OSError:
        pass


def flatten_metrics(path: Path, run: dict[str, Any]) -> dict[str, Any]:
    row = dict(run)
    row["result_dir"] = str(path.parents[1])
    row["metrics_path"] = str(path)
    if not path.exists():
        row["success"] = False
        row["error"] = "metrics.json missing"
        return row
    data = json.loads(path.read_text(encoding="utf-8"))
    for key, value in data.items():
        row[key] = value
    augment_chronolog_archive_log_metrics(row, path)
    augment_chronolog_client_append_stats(row, path)
    augment_chronolog_control_path_profiles(row, path)
    keeper_append_stats = data.get("keeper_append_stats")
    if isinstance(keeper_append_stats, dict):
        keeper_summary = keeper_append_stats.get("summary")
        if isinstance(keeper_summary, dict):
            for key, value in keeper_summary.items():
                row[f"keeper_append_{key}"] = value
    row.setdefault("operation_count_per_client", row["operation_count"])
    row.setdefault("total_operation_count", row["operation_count"] * row["client_count"])
    row.setdefault("total_payload_bytes", row["message_size_bytes"] * row["total_operation_count"])
    row.setdefault("message_count_per_client", row["operation_count_per_client"])
    row.setdefault("messages_per_client", row["operation_count_per_client"])
    row.setdefault("total_message_count", row["total_operation_count"])
    row.setdefault("total_messages", row["total_operation_count"])
    row.setdefault("parallel_client_count", row["client_count"])
    row.setdefault("parallel_clients", row["client_count"])
    row.setdefault("nodes", row["node_count"])
    for required_key in (
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
    ):
        try:
            value = int(row[required_key])
        except (KeyError, TypeError, ValueError):
            row["success"] = False
            row["benchmark_semantics_valid"] = False
            row["benchmark_validity_notes"] = (
                str(row.get("benchmark_validity_notes") or "")
                + f" Missing or invalid required metric field {required_key}."
            ).strip()
            continue
        if value <= 0:
            row["success"] = False
            row["benchmark_semantics_valid"] = False
            row["benchmark_validity_notes"] = (
                str(row.get("benchmark_validity_notes") or "")
                + f" Required metric field {required_key} must be positive."
            ).strip()
        row[required_key] = value
    row["total_payload_bytes"] = int(row["message_size_bytes"]) * int(row["total_operation_count"])
    expected_total_operation_count = int(row["operation_count_per_client"]) * int(row["client_count"])
    if int(row["total_operation_count"]) != expected_total_operation_count:
        row["success"] = False
        row["benchmark_semantics_valid"] = False
        row["benchmark_validity_notes"] = (
            str(row.get("benchmark_validity_notes") or "")
            + " total_operation_count does not match operation_count_per_client*client_count."
        ).strip()
    if int(row["operation_count"]) != int(row["operation_count_per_client"]):
        row["success"] = False
        row["benchmark_semantics_valid"] = False
        row["benchmark_validity_notes"] = (
            str(row.get("benchmark_validity_notes") or "")
            + " operation_count does not match operation_count_per_client."
        ).strip()
    for canonical, alias in (
        ("operation_count_per_client", "message_count_per_client"),
        ("operation_count_per_client", "messages_per_client"),
        ("total_operation_count", "total_message_count"),
        ("total_operation_count", "total_messages"),
        ("client_count", "parallel_client_count"),
        ("client_count", "parallel_clients"),
        ("node_count", "nodes"),
    ):
        if int(row[canonical]) != int(row[alias]):
            row["success"] = False
            row["benchmark_semantics_valid"] = False
            row["benchmark_validity_notes"] = (
                str(row.get("benchmark_validity_notes") or "")
                + f" {alias} does not match {canonical}."
            ).strip()
    refresh_keeper_append_completeness_from_logs(row, path)
    for key, value in semantic_boundary(
        row["system"],
        row["workflow"],
        str(row.get("mofka_partition_type") or ""),
        str(row.get("mofka_storage_target_type") or ""),
        str(row.get("mofka_producer_wait_mode") or ""),
        str(row.get("mofka_producer_flush_mode") or ""),
        str(row.get("chronolog_completion_mode") or ""),
        str(row.get("kafka_acks") or "1"),
    ).items():
        row.setdefault(key, value)
    if row.get("system") == "chronolog" and row.get("workflow") == "append_throughput":
        completion_mode = str(row.get("chronolog_completion_mode") or "")
        if completion_mode == "keeper_journal_buffered":
            row["semantic_boundary"] = "append_keeper_journal_buffered"
            row["append_ack_boundary"] = "record_event_return_after_keeper_journal_write"
            row["durability_boundary"] = "keeper_local_journal_buffered_not_fsync"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append returns after writing a Keeper-local journal record without fdatasync; compare only "
                "to similarly buffered local-journal semantics, not durable storage."
            )
        elif completion_mode == "keeper_journal_fdatasync":
            row["semantic_boundary"] = "append_keeper_journal_fdatasync"
            row["append_ack_boundary"] = "record_event_return_after_keeper_journal_fdatasync"
            row["durability_boundary"] = "keeper_local_journal_fdatasync"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append returns after a Keeper-local journal record is fdatasync'ed; recovery/readback is "
                "not implemented yet, so this is an initial local durability boundary, not full archive durability."
            )
        elif completion_mode == "keeper_journal_fdatasync_tail_only":
            row["semantic_boundary"] = "append_keeper_journal_fdatasync_tail_only"
            row["append_ack_boundary"] = "record_event_return_after_keeper_journal_fdatasync_no_timeline_ingest"
            row["durability_boundary"] = "keeper_local_journal_fdatasync"
            row["read_path"] = "keeper_local_journal_tail"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append returns after the Keeper-local journal record is fdatasync'ed and skips legacy "
                "timeline/archive ingestion. This is strict Keeper-local durable-tail evidence, not archive/storage readback."
            )
        elif completion_mode == "keeper_journal_group_fdatasync":
            row["semantic_boundary"] = "append_keeper_journal_periodic_fdatasync"
            row["append_ack_boundary"] = "record_event_return_after_keeper_journal_write_group_fsync_periodic"
            row["durability_boundary"] = "keeper_local_journal_periodic_fdatasync_not_per_record"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append writes to the Keeper-local journal and fdatasyncs each shard only after the "
                "configured batch event count; non-boundary appends return before their batch is forced, so compare "
                "only to similarly periodic/deferred semantics unless a restart/readback gate is part of the workflow."
            )
        elif completion_mode == "keeper_journal_group_fdatasync_early_ack":
            row["semantic_boundary"] = "append_keeper_journal_periodic_fdatasync_early_ack"
            row["append_ack_boundary"] = "record_event_return_after_keeper_journal_write_periodic_fsync_before_ingestion_drain"
            row["durability_boundary"] = "keeper_local_journal_periodic_fdatasync_not_per_record"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append writes to the Keeper-local journal, acknowledges after the local write/periodic fdatasync path, "
                "and then feeds the old ingestion/timeline path after responding. This is a journal-first acceptance "
                "probe; require journal parser and post-ack ingestion-drain counters before using it. Non-boundary records "
                "do not wait for per-record fdatasync."
            )
        elif completion_mode == "keeper_journal_group_fdatasync_async_drain":
            row["semantic_boundary"] = "append_keeper_journal_periodic_fdatasync_async_drain"
            row["append_ack_boundary"] = "record_event_return_after_keeper_journal_write_periodic_fsync_and_memory_drain_enqueue"
            row["durability_boundary"] = "keeper_local_journal_periodic_fdatasync_not_per_record"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append writes to the Keeper-local journal, acknowledges after the local write/periodic fdatasync path, "
                "and enqueues a memory copy for the old ingestion/timeline feed to a Keeper-side async drain thread. Require journal "
                "parser, async drain complete count, and post-ack ingestion counters before using it. Non-boundary records "
                "do not wait for per-record fdatasync."
            )
        elif completion_mode == "keeper_journal_group_fdatasync_wal_drain":
            row["semantic_boundary"] = "append_keeper_journal_periodic_fdatasync_wal_drain"
            row["append_ack_boundary"] = "record_event_return_after_keeper_journal_write_periodic_fsync_and_wal_cursor_enqueue"
            row["durability_boundary"] = "keeper_local_journal_periodic_fdatasync_not_per_record"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append writes to the Keeper-local journal, acknowledges after the local write/periodic fdatasync path, "
                "and enqueues a WAL cursor for the old ingestion/timeline feed. The async drain rereads the acknowledged "
                "record from the Keeper WAL before feeding the legacy timeline/archive path. Require journal parser, "
                "async drain complete count, and post-ack ingestion counters before using it. Non-boundary records do not "
                "wait for per-record fdatasync."
            )
        elif completion_mode == "keeper_journal_group_fdatasync_tail_only":
            row["semantic_boundary"] = "append_keeper_journal_periodic_fdatasync_tail_only"
            row["append_ack_boundary"] = "record_event_return_after_keeper_journal_write_periodic_fsync_no_timeline_ingest"
            row["durability_boundary"] = "keeper_local_journal_periodic_fdatasync_not_per_record"
            row["read_path"] = "keeper_local_journal_tail"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append writes to the Keeper-local journal and acknowledges after the local write/periodic fdatasync path "
                "without feeding the legacy in-memory timeline/archive path. This is a Keeper-local durable tail semantic "
                "only after a restart/readback gate; non-boundary records do not wait for per-record fdatasync. Do not "
                "compare it to archive/storage readback or use it as evidence that Grapher/PFS drain completed."
            )
        elif completion_mode == "keeper_journal_group_commit_tail_only":
            row["semantic_boundary"] = "append_keeper_journal_group_commit_tail_only"
            row["append_ack_boundary"] = (
                "record_event_return_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest"
            )
            row["durability_boundary"] = "keeper_local_journal_group_commit_fdatasync"
            row["read_path"] = "keeper_local_journal_tail"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append is served by a single Keeper journal owner per shard; the owner writes a batch, "
                "fdatasyncs once, then completes all appends in that batch. The legacy in-memory timeline/archive "
                "consumer is skipped, so this is Keeper-local durable-tail evidence, not archive/storage readback."
            )
        elif completion_mode == "keeper_journal_group_commit_deferred_tail_only":
            row["semantic_boundary"] = "append_keeper_journal_group_commit_deferred_rpc_tail_only"
            row["append_ack_boundary"] = (
                "record_event_response_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest"
            )
            row["durability_boundary"] = "keeper_local_journal_group_commit_fdatasync"
            row["read_path"] = "keeper_local_journal_tail"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog append RPC response is deferred until the single-writer Keeper journal owner writes "
                "a batch, fdatasyncs once, then responds to all appends in that batch. The legacy in-memory "
                "timeline/archive consumer is skipped, so this is Keeper-local durable-tail evidence, not "
                "archive/storage readback."
            )
    if row.get("system") == "chronolog" and row.get("workflow") == "keeper_restart_recovery":
        completion_mode = str(row.get("chronolog_completion_mode") or "")
        if completion_mode == "keeper_journal_group_fdatasync":
            row["semantic_boundary"] = "keeper_restart_recovery_periodic_fdatasync_journal_tail"
            row["append_ack_boundary"] = "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync"
            row["durability_boundary"] = "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart"
            row["read_path"] = "keeper_tail_rpc_recovered_journal_index"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync batching, "
                "restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes. "
                "Append return is not per-record fsync wait for non-boundary records; durability evidence is the restart/readback gate."
            )
        elif completion_mode == "keeper_journal_group_fdatasync_wal_drain":
            row["semantic_boundary"] = "keeper_restart_recovery_periodic_fdatasync_wal_drain_journal_tail"
            row["append_ack_boundary"] = "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync_and_wal_cursor_enqueue"
            row["durability_boundary"] = "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart"
            row["read_path"] = "keeper_tail_rpc_recovered_journal_index"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync batching and WAL-cursor "
                "async drain semantics, restarts Keepers, then reads the same story through Keeper tail RPCs backed by "
                "recovered local journal indexes. Append return is not per-record fsync wait for non-boundary records; "
                "durability evidence is the restart/readback gate."
            )
        elif completion_mode == "keeper_journal_group_fdatasync_tail_only":
            row["semantic_boundary"] = "keeper_restart_recovery_periodic_fdatasync_tail_only_journal_tail"
            row["append_ack_boundary"] = "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync_no_timeline_ingest"
            row["durability_boundary"] = "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart"
            row["read_path"] = "keeper_tail_rpc_recovered_journal_index"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync tail-only semantics, "
                "skips the legacy timeline/archive consumer, restarts Keepers, then reads the same story through Keeper tail "
                "RPCs backed by recovered local journal indexes. Append return is not per-record fsync wait for non-boundary "
                "records; durability evidence is the restart/readback gate. This is not archive/storage readback evidence."
            )
        elif completion_mode == "keeper_journal_group_commit_tail_only":
            row["semantic_boundary"] = "keeper_restart_recovery_group_commit_tail_only_journal_tail"
            row["append_ack_boundary"] = (
                "StoryHandle.log_event_return_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest"
            )
            row["durability_boundary"] = "keeper_local_journal_group_commit_fdatasync_recovered_after_keeper_restart"
            row["read_path"] = "keeper_tail_rpc_recovered_journal_index"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog writes through normal client RPCs to a single Keeper journal owner per shard. The owner "
                "writes a batch, fdatasyncs once, then completes all appends in that batch; legacy timeline/archive "
                "ingestion is skipped. Keepers are restarted and the same story is read through recovered Keeper "
                "journal indexes. This is not archive/storage readback evidence."
            )
        elif completion_mode == "keeper_journal_group_commit_deferred_tail_only":
            row["semantic_boundary"] = "keeper_restart_recovery_group_commit_deferred_rpc_tail_only_journal_tail"
            row["append_ack_boundary"] = (
                "StoryHandle.log_event_return_after_deferred_keeper_journal_group_commit_fdatasync_no_timeline_ingest"
            )
            row["durability_boundary"] = "keeper_local_journal_group_commit_fdatasync_recovered_after_keeper_restart"
            row["read_path"] = "keeper_tail_rpc_recovered_journal_index"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog writes through normal client RPCs to a single Keeper journal owner per shard. The RPC "
                "response is deferred until the owner writes a batch and completes a covering fdatasync; legacy "
                "timeline/archive ingestion is skipped. Keepers are restarted and the same story is read through "
                "recovered Keeper journal indexes. This is not archive/storage readback evidence."
            )
        elif completion_mode == "keeper_journal_fdatasync":
            row["semantic_boundary"] = "keeper_restart_recovery_fdatasync_journal_tail"
            row["append_ack_boundary"] = "StoryHandle.log_event_return_after_keeper_journal_fdatasync"
            row["durability_boundary"] = "keeper_local_journal_fdatasync_recovered_after_keeper_restart"
            row["read_path"] = "keeper_tail_rpc_recovered_journal_index"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog writes through normal client RPCs with strict Keeper-local journal fdatasync semantics, "
                "restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes."
            )
        elif completion_mode == "keeper_journal_fdatasync_tail_only":
            row["semantic_boundary"] = "keeper_restart_recovery_fdatasync_tail_only_journal_tail"
            row["append_ack_boundary"] = "StoryHandle.log_event_return_after_keeper_journal_fdatasync_no_timeline_ingest"
            row["durability_boundary"] = "keeper_local_journal_fdatasync_recovered_after_keeper_restart"
            row["read_path"] = "keeper_tail_rpc_recovered_journal_index"
            row["storage_backend"] = "chronolog_keeper_local_journal"
            row["semantic_notes"] = (
                "ChronoLog writes through normal client RPCs with strict Keeper-local journal fdatasync tail-only "
                "semantics, skips legacy timeline/archive ingestion, restarts Keepers, then reads the same story through "
                "Keeper tail RPCs backed by recovered local journal indexes. This is not archive/storage readback evidence."
            )
    if (
        row.get("system") == "chronolog"
        and row.get("workflow") in {"range_retrieval", "mixed_append_tail"}
        and str(row.get("keeper_tail_source") or "") == "journal"
    ):
        mixed_tail_mode = row.get("tail_read_mode") or os.environ.get("CHRONOLOG_MIXED_TAIL_READ_MODE") or "full"
        mixed_tail_start = row.get("tail_reader_start_mode") or row.get("chronolog_mixed_tail_reader_start_mode") or "ready"
        tail_batch_max_bytes = str(row.get("keeper_tail_batch_max_bytes") or "").strip()
        tail_batch_max_events = str(row.get("keeper_tail_batch_max_events") or "").strip()
        if mixed_tail_mode == "keeper_cursor" and (
            tail_batch_max_bytes not in {"", "0"} or tail_batch_max_events not in {"", "0"}
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
            mixed_tail_mode = f"{mixed_tail_mode}_bounded_tail_batch_{'_'.join(suffix_parts) or 'bounded'}"
        row["semantic_boundary"] = (
            (
                f"mixed_append_concurrent_keeper_journal_tail_{mixed_tail_mode}"
                if mixed_tail_start == "ready"
                else f"append_then_keeper_journal_tail_catchup_{mixed_tail_mode}"
            )
            if row.get("workflow") == "mixed_append_tail"
            else "range_retrieval_keeper_journal_tail"
        )
        row["durability_boundary"] = f"keeper_local_journal_{row.get('keeper_journal_mode') or 'unknown'}"
        row["read_path"] = (
            (
                "concurrent_keeper_local_journal_tail"
                if mixed_tail_start == "ready"
                else "keeper_local_journal_tail_catchup_after_append"
            )
            if row.get("workflow") == "mixed_append_tail"
            else "keeper_local_journal_tail"
        )
        row["storage_backend"] = "chronolog_keeper_local_journal"
        row["semantic_notes"] = (
            "ChronoLog ReplayStory is served from Keeper-local journal tail records before archive fallback; validate "
            "retrieved_event_count, live_tail_succeeded, and journal parser fields before comparing."
        )
        if "bounded_tail_batch" in mixed_tail_mode:
            row["semantic_notes"] += (
                " Keeper cursor tail retrieval used bounded per-Keeper journal-tail RPC batches, so this is a "
                "streaming/cursor catch-up semantic and must not be merged with full ReplayStory rows."
            )
    elif row.get("system") == "chronolog" and row.get("workflow") == "mixed_append_tail":
        mixed_tail_mode = row.get("tail_read_mode") or os.environ.get("CHRONOLOG_MIXED_TAIL_READ_MODE") or "full"
        mixed_tail_start = row.get("tail_reader_start_mode") or row.get("chronolog_mixed_tail_reader_start_mode") or "ready"
        row["semantic_boundary"] = (
            f"mixed_append_concurrent_live_tail_{mixed_tail_mode}"
            if mixed_tail_start == "ready"
            else f"append_then_live_tail_catchup_{mixed_tail_mode}"
        )
        row["append_ack_boundary"] = "chrono_bench_record_event_return"
        row["durability_boundary"] = "not_proven_durable"
        row["read_path"] = (
            "concurrent_live_tail"
            if mixed_tail_start == "ready"
            else "live_tail_catchup_after_append"
        )
        row["storage_backend"] = "not_proven_durable"
        row["semantic_notes"] = (
            "ChronoLog ReplayStory is served from the live path with no durable/storage completion proof; validate "
            "live_tail_succeeded and retrieved counts before comparing."
        )
    annotate_benchmark_validity(row)
    row["error"] = ""
    return row


def scan_mofka_service_health(child_dir: Path) -> tuple[bool, str]:
    log_dir = child_dir / "mofka" / "logs"
    if not log_dir.exists():
        return True, ""

    hard_fatal_patterns = (
        "Segmentation fault",
        "core dumped",
        "Process received signal",
        "Address not mapped",
    )
    forced_shutdown_patterns = (
        "srun: forcing job termination",
        "Job step aborted",
        "CANCELLED",
        "Killed",
    )
    findings: list[str] = []
    forced_shutdown_findings: list[str] = []
    for log_path in sorted(log_dir.glob("*.stderr.log")):
        try:
            text = log_path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            findings.append(f"{log_path.name}: unreadable stderr log: {exc}")
            continue
        for pattern in hard_fatal_patterns:
            if pattern in text:
                findings.append(f"{log_path.name}: contains {pattern!r}")
                break
        else:
            for pattern in forced_shutdown_patterns:
                if pattern in text:
                    forced_shutdown_findings.append(f"{log_path.name}: contains {pattern!r}")
                    break

    if findings:
        return False, "; ".join(findings)
    if forced_shutdown_findings:
        return True, "forced service shutdown observed after benchmark: " + "; ".join(forced_shutdown_findings)
    return True, ""


def scan_chronolog_service_health(child_dir: Path) -> tuple[bool, str]:
    log_dir = child_dir / "chronolog" / "logs"
    if not log_dir.exists():
        return True, ""

    hard_fatal_patterns = (
        "Segmentation fault",
        "core dumped",
        "Caught signal 11",
        "Process received signal",
        "Address not mapped",
    )
    findings: list[str] = []
    for log_path in sorted(log_dir.glob("*.log")):
        try:
            text = log_path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            findings.append(f"{log_path.name}: unreadable log: {exc}")
            continue
        for pattern in hard_fatal_patterns:
            if pattern in text:
                findings.append(f"{log_path.name}: contains {pattern!r}")
                break

    if findings:
        return False, "; ".join(findings)
    return True, ""


def apply_chronolog_service_health(row: dict[str, Any], child_dir: Path) -> None:
    if row.get("system") != "chronolog":
        return
    healthy, notes = scan_chronolog_service_health(child_dir)
    row["chronolog_service_health_ok"] = healthy
    row["chronolog_service_health_notes"] = notes
    if healthy:
        return
    row["success"] = False
    existing_error = str(row.get("error") or "").strip()
    row["error"] = f"{existing_error} ChronoLog service health check failed.".strip()
    existing_notes = str(row.get("benchmark_validity_notes") or "").strip()
    health_note = f"ChronoLog service log health check failed: {notes}"
    row["benchmark_validity_notes"] = f"{existing_notes} {health_note}".strip()
    row["benchmark_semantics_valid"] = False


def apply_mofka_service_health(row: dict[str, Any], child_dir: Path) -> None:
    if row.get("system") != "mofka":
        return
    healthy, notes = scan_mofka_service_health(child_dir)
    row["mofka_service_health_ok"] = healthy
    row["mofka_service_health_notes"] = notes
    if healthy:
        return
    row["success"] = False
    existing_error = str(row.get("error") or "").strip()
    row["error"] = f"{existing_error} Mofka service health check failed.".strip()
    existing_notes = str(row.get("benchmark_validity_notes") or "").strip()
    health_note = f"Mofka service stderr health check failed: {notes}"
    row["benchmark_validity_notes"] = f"{existing_notes} {health_note}".strip()
    row["benchmark_semantics_valid"] = False


def truthy(value: Any) -> bool:
    return str(value).strip().lower() in {"1", "true", "yes", "on"}


def annotate_benchmark_validity(row: dict[str, Any]) -> None:
    notes: list[str] = []
    valid = True
    success_value = row.get("success")
    if success_value not in {None, ""} and str(success_value).strip().lower() not in {"true", "1", "dry-run"}:
        valid = False
        notes.append("Benchmark command did not report success.")
    journal_tail_range = (
        row.get("system") == "chronolog"
        and row.get("workflow") in {"range_retrieval", "mixed_append_tail"}
        and str(row.get("keeper_tail_source") or "") == "journal"
    )
    keeper_restart_recovery = (
        row.get("system") == "chronolog" and row.get("workflow") == "keeper_restart_recovery"
    )
    total_operation_count = int(float(row.get("total_operation_count") or 0))
    journal_tail_range_complete = False
    keeper_restart_complete = False
    chronolog_archive_complete = False
    if row.get("system") == "chronolog" and row.get("workflow") == "append_durable":
        try:
            archive_count = int(float(row.get("archive_event_count") or 0))
        except (TypeError, ValueError):
            archive_count = 0
        try:
            readback_count = int(float(row.get("readback_event_count") or 0))
        except (TypeError, ValueError):
            readback_count = 0
        chronolog_archive_complete = archive_count >= total_operation_count and readback_count >= total_operation_count
        if not chronolog_archive_complete:
            valid = False
            notes.append("ChronoLog archive storage row did not confirm all expected archived/readback events.")
    if row.get("system") == "chronolog" and row.get("workflow") == "archive_range_retrieval":
        try:
            archive_count = int(float(row.get("archive_event_count") or 0))
        except (TypeError, ValueError):
            archive_count = 0
        try:
            retrieved_count = int(float(row.get("retrieved_event_count") or 0))
        except (TypeError, ValueError):
            retrieved_count = 0
        try:
            expected_retrieved_count = int(float(row.get("expected_retrieved_event_count") or 0))
        except (TypeError, ValueError):
            expected_retrieved_count = 0
        chronolog_archive_complete = archive_count >= total_operation_count and (
            expected_retrieved_count > 0 and retrieved_count == expected_retrieved_count
        )
        if not chronolog_archive_complete:
            valid = False
            notes.append("ChronoLog archive range row did not confirm full archive event count and exact range readback.")
    if journal_tail_range:
        try:
            retrieved_count = int(float(row.get("retrieved_event_count") or row.get("tail_max_retrieved_count") or 0))
        except (TypeError, ValueError):
            retrieved_count = 0
        try:
            expected_retrieved_count = int(float(row.get("expected_retrieved_event_count") or total_operation_count))
        except (TypeError, ValueError):
            expected_retrieved_count = total_operation_count
        live_tail_complete = truthy(row.get("live_tail_succeeded")) and retrieved_count >= expected_retrieved_count
        journal_complete = truthy(row.get("keeper_journal_valid")) and truthy(
            row.get("keeper_journal_record_count_matches_total")
        )
        journal_tail_range_complete = live_tail_complete and journal_complete
        if not live_tail_complete:
            valid = False
            notes.append("Keeper journal-tail range row did not retrieve all expected live-tail events.")
    if keeper_restart_recovery:
        try:
            retrieved_count = int(float(row.get("retrieved_event_count") or 0))
        except (TypeError, ValueError):
            retrieved_count = 0
        live_tail_complete = truthy(row.get("live_tail_succeeded")) and retrieved_count >= total_operation_count
        restart_complete = truthy(row.get("keeper_restart_command_success")) and truthy(
            row.get("keeper_restart_health_success")
        )
        journal_complete = truthy(row.get("keeper_journal_valid")) and truthy(
            row.get("keeper_journal_record_count_matches_total")
        )
        keeper_restart_complete = live_tail_complete and restart_complete and journal_complete
        if not live_tail_complete:
            valid = False
            notes.append("Keeper restart recovery row did not retrieve all expected events after restart.")
        if not restart_complete:
            valid = False
            notes.append("Keeper restart command or health gate failed.")
    if (
        row.get("system") == "chronolog"
        and row.get("workflow") == "append_throughput"
        and int(row.get("client_count") or 1) > 1
        and truthy(row.get("chronolog_chrono_bench_shared_story"))
        and not truthy(row.get("chronolog_chrono_bench_barrier"))
    ):
        valid = False
        notes.append(
            "ChronoLog shared-story multi-client append without chrono-bench barrier can release/destroy the story while other ranks are still writing."
        )
    keeper_complete = row.get("keeper_append_append_stats_complete")
    if (
        not journal_tail_range_complete
        and not keeper_restart_complete
        and not chronolog_archive_complete
        and keeper_complete is not None
        and str(keeper_complete).strip() not in {"", "True", "true", "1"}
    ):
        valid = False
        notes.append("Keeper append stats are incomplete or do not match expected total operations.")
    orphan_warnings = row.get("keeper_append_orphan_warning_count")
    try:
        if orphan_warnings not in {None, ""} and int(float(orphan_warnings)) > 0:
            valid = False
            notes.append("Keeper orphan-event warnings were observed.")
    except (TypeError, ValueError):
        pass
    try:
        grapher_orphans = int(float(row.get("grapher_orphan_chunk_count") or 0))
    except (TypeError, ValueError):
        grapher_orphans = 0
    if grapher_orphans > 0:
        if row.get("system") == "chronolog" and row.get("workflow") == "append_throughput":
            notes.append(
                "Grapher orphan chunks were observed; this row is valid only for the live append-return boundary, not for archive/storage evidence."
            )
        else:
            valid = False
            notes.append("Grapher orphan chunks were observed.")
    if str(row.get("keeper_journal_mode") or "disabled") != "disabled":
        if str(row.get("keeper_journal_valid")).strip() not in {"True", "true", "1"}:
            valid = False
            notes.append("Keeper journal parser reported an invalid journal.")
        if str(row.get("keeper_journal_record_count_matches_total")).strip() not in {"True", "true", "1"}:
            valid = False
            notes.append("Keeper journal record count does not match expected total operations.")
    if (
        row.get("system") == "chronolog"
        and not keeper_restart_complete
        and str(row.get("chronolog_completion_mode") or "")
        in {"keeper_journal_group_fdatasync_early_ack", "keeper_journal_group_fdatasync_async_drain", "keeper_journal_group_fdatasync_wal_drain"}
    ):
        try:
            post_ack_count = int(float(row.get("keeper_append_post_ack_ingest_count") or 0))
        except (TypeError, ValueError):
            post_ack_count = 0
        if post_ack_count < total_operation_count:
            valid = False
            notes.append("Post-ack ingestion drain count does not match expected total operations.")
    if (
        row.get("system") == "chronolog"
        and not keeper_restart_complete
        and str(row.get("chronolog_completion_mode") or "")
        in {"keeper_journal_group_fdatasync_async_drain", "keeper_journal_group_fdatasync_wal_drain"}
    ):
        try:
            async_drain_count = int(float(row.get("keeper_append_async_drain_complete_count") or 0))
        except (TypeError, ValueError):
            async_drain_count = 0
        if async_drain_count < total_operation_count:
            valid = False
            notes.append("Async drain completion count does not match expected total operations.")
    if (
        row.get("system") == "chronolog"
        and str(row.get("chronolog_completion_mode") or "") == "keeper_journal_group_commit_deferred_tail_only"
        and str(row.get("keeper_journal_durable_complete_before_publish") or row.get(
            "chronolog_keeper_journal_durable_complete_before_publish"
        ) or "0")
        == "1"
    ):
        row["append_ack_boundary"] = (
            "deferred_rpc_response_after_keeper_journal_group_commit_fdatasync_before_tail_publish"
        )
        row["semantic_notes"] = (
            str(row.get("semantic_notes") or "").rstrip()
            + " With keeper_journal_durable_complete_before_publish=1, the RPC may complete after the journal "
              "fdatasync and before the Keeper tail index is published; durability is WAL-backed, while immediate "
              "live-tail visibility is a separate post-ack publication step."
        ).strip()
    row["benchmark_semantics_valid"] = valid
    row["benchmark_validity_notes"] = " ".join(notes)


def write_summary(rows: list[dict[str, Any]], path: Path) -> None:
    keys = [
        "system",
        "workflow",
        "trial",
        "node_count",
        "nodes",
        "client_count",
        "parallel_clients",
        "message_size_bytes",
        "operation_count",
        "operation_count_per_client",
        "message_count_per_client",
        "messages_per_client",
        "total_operation_count",
        "total_message_count",
        "total_messages",
        "total_payload_bytes",
        "parallel_client_count",
        "semantic_boundary",
        "append_ack_boundary",
        "durability_boundary",
        "read_path",
        "storage_backend",
        "chronolog_completion_mode",
        "chronolog_data_collection_poll_interval_us",
        "chronolog_archive_event_count_poll_interval_seconds",
        "chronolog_archive_readback_mode",
        "chronolog_keeper_data_collection_streams",
        "chronolog_keeper_data_collection_threads_per_stream",
        "chronolog_grapher_data_collection_streams",
        "chronolog_grapher_data_collection_threads_per_stream",
        "chronolog_grapher_inactive_story_delay_seconds",
        "chronolog_grapher_retire_on_stop",
        "chronolog_grapher_stop_retire_grace_us",
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
        "chronolog_archive_range_event_count",
        "chronolog_perf_event",
        "chronolog_perf_call_graph",
        "chronolog_profile_valid",
        "chronolog_profile_notes",
        "chronolog_profile_artifact_count",
        "chronolog_profile_nonempty_artifact_count",
        "chronolog_profile_error_file_count",
        "chronolog_profile_error_nonempty_count",
        "chronolog_perf_data_file_count",
        "chronolog_perf_nonempty_data_file_count",
        "chronolog_ebpf_output_file_count",
        "chronolog_ebpf_nonempty_output_file_count",
        "chronolog_gperftools_output_file_count",
        "chronolog_gperftools_nonempty_output_file_count",
        "chronolog_chrono_bench_barrier",
        "chronolog_chrono_bench_shared_story",
        "chronolog_producer_outstanding",
        "chronolog_producer_batch_size",
        "chronolog_producer_wait_policy",
        "chronolog_client_batch_keeper_selection",
        "chronolog_client_keeper_time_bucket_ns",
        "chronolog_client_parallel_tail_rpc",
        "chronolog_client_keeper_cursor_drain",
        "chronolog_client_keeper_cursor_drain_max_batches",
        "chronolog_client_keeper_cursor_packed_batch",
        "chronolog_client_keeper_cursor_packed_bulk",
        "chronolog_client_keeper_cursor_packed_bulk_stream",
        "chronolog_client_keeper_cursor_packed_bulk_stream_max_batches",
        "chronolog_client_keeper_cursor_packed_bulk_buffer_bytes",
        "chronolog_mixed_tail_read_mode",
        "chronolog_mixed_tail_reader_start_mode",
        "tail_read_mode",
        "tail_reader_start_mode",
        "chronolog_client_execution_mode",
        "chronolog_rpc_port_offset",
        "chronolog_producer_wait_mode",
        "chronolog_keeper_journal_shards",
        "chronolog_keeper_journal_shard_policy",
        "chronolog_keeper_journal_placement",
        "chronolog_keeper_journal_local_base",
        "chronolog_keeper_journal_fdatasync_batch_events",
        "chronolog_keeper_journal_batch_writev",
        "chronolog_keeper_journal_move_batch_payloads",
        "chronolog_keeper_tail_batch_max_events",
        "chronolog_keeper_tail_batch_max_bytes",
        "chronolog_keeper_journal_group_commit_flush_events",
        "chronolog_keeper_journal_group_commit_strict_flush_event_cap",
        "chronolog_keeper_journal_group_commit_flush_bytes",
        "chronolog_keeper_journal_group_commit_large_payload_bytes",
        "chronolog_keeper_journal_group_commit_large_payload_flush_events",
        "chronolog_keeper_journal_group_commit_wait_us",
        "chronolog_keeper_journal_group_commit_flush_wait_us",
        "chronolog_keeper_append_stats_interval_events",
        "chronolog_keeper_wal_drain_batch_events",
        "chronolog_keeper_wal_drain_batch_wait_us",
        "chronolog_keeper_journal_async_drain_threads",
        "chronolog_keeper_journal_async_callback_dispatch",
        "chronolog_keeper_journal_async_batch_completion_dispatch",
        "chronolog_keeper_journal_callback_dispatch_threads",
        "chronolog_keeper_journal_callback_batch_drain",
        "chronolog_keeper_journal_callback_batch_drain_max",
        "chronolog_keeper_journal_callback_batch_drain_min_payload_bytes",
        "chronolog_keeper_journal_durable_complete_before_publish",
        "chronolog_keeper_recording_margo_xstreams",
        "chronolog_keeper_recording_margo_progress_thread",
        "chronolog_keeper_recording_margo_handlers",
        "chronolog_grapher_recording_margo_xstreams",
        "chronolog_grapher_recording_margo_handlers",
        "chronolog_grapher_direct_deserialize",
        "chronolog_keeper_drain_margo_progress_thread",
        "chronolog_keeper_drain_margo_rpc_threads",
        "chronolog_keeper_extraction_threads",
        "chronolog_keeper_direct_serialize",
        "chronolog_keeper_stop_story_flush_drain",
        "chronolog_keeper_stop_story_flush_drain_timeout_ms",
        "chronolog_visor_parallel_keeper_stop",
        "client_mpi_oversubscribe",
        "keeper_journal_shards",
        "keeper_journal_shard_policy",
        "keeper_journal_placement",
        "keeper_journal_local_base",
        "keeper_journal_run_id",
        "keeper_journal_fdatasync_batch_events",
        "keeper_wal_drain_batch_events",
        "keeper_wal_drain_batch_wait_us",
        "keeper_journal_atomic_offsets",
        "keeper_journal_single_writer",
        "keeper_journal_single_writer_batch_events",
        "keeper_journal_group_commit_wait",
        "keeper_journal_group_commit_flush_events",
        "keeper_journal_group_commit_flush_bytes",
        "keeper_journal_group_commit_large_payload_bytes",
        "keeper_journal_group_commit_large_payload_flush_events",
        "keeper_journal_group_commit_wait_us",
        "keeper_journal_group_commit_flush_wait_us",
        "keeper_journal_owner_drain_yields",
        "keeper_journal_async_callback_dispatch",
        "keeper_journal_async_batch_completion_dispatch",
        "keeper_journal_callback_dispatch_threads",
        "keeper_journal_callback_batch_drain",
        "keeper_journal_callback_batch_drain_max",
        "keeper_journal_callback_batch_drain_min_payload_bytes",
        "keeper_journal_durable_complete_before_publish",
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
        "keeper_journal_tail_read_mode",
        "keeper_journal_tail_direct_read_into_events",
        "keeper_tail_batch_max_events",
        "keeper_tail_batch_max_bytes",
        "keeper_append_stats_interval_events",
        "keeper_stop_story_stats_wait_ms",
        "keeper_stop_story_flush_drain",
        "keeper_stop_story_flush_drain_timeout_ms",
        "visor_parallel_keeper_stop",
        "grapher_retire_on_stop",
        "grapher_stop_story_archive_drain",
        "grapher_stop_story_archive_drain_timeout_ms",
        "grapher_extraction_threads",
        "hdf5_archive_atomic_rename",
        "hdf5_archive_chunk_events",
        "hdf5_archive_layout",
        "raw_blob_preallocate",
        "raw_blob_async_close",
        "mofka_partition_type",
        "mofka_storage_target_type",
        "mofka_storage_target_size",
        "mofka_producer_wait_mode",
        "mofka_producer_flush_mode",
        "mofka_precreate_storage_provider",
        "kafka_acks",
        "chronolog_service_health_ok",
        "chronolog_service_health_notes",
        "mofka_service_health_ok",
        "mofka_service_health_notes",
        "semantic_notes",
        "benchmark_semantics_valid",
        "benchmark_validity_notes",
        "duration_seconds",
        "throughput_ops_per_sec",
        "throughput_semantics",
        "latency_semantics",
        "workflow_duration_seconds",
        "workflow_total_message_throughput_ops_per_sec",
        "workflow_active_duration_seconds",
        "workflow_total_message_active_throughput_ops_per_sec",
        "archive_range_append_clients_seconds",
        "archive_range_metadata_selection_seconds",
        "append_duration_seconds",
        "append_throughput_ops_per_sec",
        "append_avg_latency_ms",
        "replay_duration_seconds",
        "replay_throughput_ops_per_sec",
        "range_readback_duration_seconds",
        "range_readback_throughput_ops_per_sec",
        "range_readback_event_count",
        "range_readback_latency_ms",
        "avg_latency_ms",
        "p50_latency_ms",
        "p95_latency_ms",
        "p99_latency_ms",
        "archive_wait_seconds",
        "archive_log_settle_seconds",
        "archive_wait_started_epoch_seconds",
        "archive_file_detect_seconds",
        "archive_file_detect_epoch_seconds",
        "archive_file_mtime_epoch_seconds",
        "release_to_archive_file_mtime_seconds",
        "archive_file_mtime_to_detect_seconds",
        "archive_file_size_bytes",
        "archive_sidecar_file_size_bytes",
        "archive_total_file_size_bytes",
        "archive_event_count_confirm_seconds",
        "archive_event_count_confirm_epoch_seconds",
        "archive_event_count_poll_interval_seconds",
        "archive_event_count_poll_count",
        "archive_event_count",
        "archive_error",
        "range_start_ns",
        "range_end_ns",
        "range_event_count",
        "retrieved_event_count",
        "expected_retrieved_event_count",
        "timestamp_dataset",
        "readback_event_count",
        "readback_path",
        "client_phase_config_loaded_seconds_max",
        "client_phase_connect_returned_seconds_max",
        "client_phase_create_chronicle_returned_seconds_max",
        "client_phase_acquire_story_returned_seconds_max",
        "client_phase_append_loop_finished_seconds_max",
        "client_phase_release_story_returned_seconds_max",
        "release_seconds",
        "story_id",
        "keeper_to_grapher_drain_chunk_count",
        "keeper_to_grapher_drain_first_epoch_seconds",
        "keeper_to_grapher_drain_last_epoch_seconds",
        "release_to_keeper_to_grapher_drain_first_seconds",
        "release_to_keeper_to_grapher_drain_last_seconds",
        "keeper_flush_story_start_first_epoch_seconds",
        "keeper_flush_story_complete_last_epoch_seconds",
        "release_to_keeper_flush_story_start_first_seconds",
        "release_to_keeper_flush_story_complete_last_seconds",
        "keeper_flush_story_max_seconds",
        "keeper_flush_story_count",
        "keeper_flush_finalize_max_us",
        "keeper_flush_drain_wait_max_us",
        "keeper_flush_total_max_us",
        "keeper_drain_profile_count",
        "keeper_drain_profile_events",
        "keeper_drain_profile_bytes",
        "keeper_drain_profile_serialization_us",
        "keeper_drain_profile_serialization_avg_us",
        "keeper_drain_profile_serialization_max_us",
        "keeper_drain_profile_serialization_min_us",
        "keeper_drain_profile_rpc_us",
        "keeper_drain_profile_rpc_avg_us",
        "keeper_drain_profile_rpc_max_us",
        "keeper_drain_profile_rpc_min_us",
        "keeper_drain_profile_total_us",
        "keeper_drain_profile_total_avg_us",
        "keeper_drain_profile_total_max_us",
        "keeper_drain_profile_total_min_us",
        "rdma_transfer_profile_count",
        "rdma_transfer_profile_bytes",
        "rdma_transfer_profile_transferred_bytes",
        "rdma_transfer_profile_expose_us",
        "rdma_transfer_profile_remote_call_us",
        "rdma_transfer_profile_total_us",
        "grapher_receive_profile_count",
        "grapher_receive_profile_events",
        "grapher_receive_profile_bytes",
        "grapher_receive_profile_allocation_us",
        "grapher_receive_profile_allocation_avg_us",
        "grapher_receive_profile_allocation_max_us",
        "grapher_receive_profile_allocation_min_us",
        "grapher_receive_profile_local_bulk_expose_us",
        "grapher_receive_profile_local_bulk_expose_avg_us",
        "grapher_receive_profile_local_bulk_expose_max_us",
        "grapher_receive_profile_local_bulk_expose_min_us",
        "grapher_receive_profile_bulk_transfer_us",
        "grapher_receive_profile_bulk_transfer_avg_us",
        "grapher_receive_profile_bulk_transfer_max_us",
        "grapher_receive_profile_bulk_transfer_min_us",
        "grapher_receive_profile_deserialization_us",
        "grapher_receive_profile_deserialization_avg_us",
        "grapher_receive_profile_deserialization_max_us",
        "grapher_receive_profile_deserialization_min_us",
        "grapher_receive_profile_ingestion_us",
        "grapher_receive_profile_ingestion_avg_us",
        "grapher_receive_profile_ingestion_max_us",
        "grapher_receive_profile_ingestion_min_us",
        "grapher_receive_profile_response_us",
        "grapher_receive_profile_response_avg_us",
        "grapher_receive_profile_response_max_us",
        "grapher_receive_profile_response_min_us",
        "grapher_receive_profile_total_before_response_us",
        "grapher_receive_profile_total_before_response_avg_us",
        "grapher_receive_profile_total_before_response_max_us",
        "grapher_receive_profile_total_before_response_min_us",
        "grapher_receive_profile_total_with_response_us",
        "grapher_receive_profile_total_with_response_avg_us",
        "grapher_receive_profile_total_with_response_max_us",
        "grapher_receive_profile_total_with_response_min_us",
        "grapher_archive_drain_count",
        "grapher_archive_drain_finalize_max_us",
        "grapher_archive_drain_wait_max_us",
        "grapher_archive_drain_wait_total_us",
        "grapher_archive_drain_queued_nonzero_count",
        "grapher_archive_drain_inflight_nonzero_count",
        "grapher_story_pipeline_merge_profile_count",
        "grapher_story_pipeline_merge_profile_source_events",
        "grapher_story_pipeline_merge_profile_merged_events",
        "grapher_story_pipeline_merge_profile_remaining_events",
        "grapher_story_pipeline_merge_profile_total_us",
        "grapher_hdf5_write_profile_count",
        "grapher_hdf5_write_profile_events",
        "grapher_hdf5_write_profile_file_size_bytes",
        "grapher_hdf5_write_profile_write_us",
        "grapher_hdf5_subphase_profile_count",
        "grapher_hdf5_subphase_prep_us",
        "grapher_hdf5_subphase_prep_scan_us",
        "grapher_hdf5_subphase_prep_build_us",
        "grapher_hdf5_subphase_prep_payload_copy_us",
        "grapher_hdf5_subphase_lock_wait_us",
        "grapher_hdf5_subphase_filename_scan_us",
        "grapher_hdf5_subphase_open_us",
        "grapher_hdf5_subphase_dataset_write_us",
        "grapher_hdf5_subphase_dataset_group_create_us",
        "grapher_hdf5_subphase_dataset_dataspace_create_us",
        "grapher_hdf5_subphase_dataset_datatype_create_us",
        "grapher_hdf5_subphase_dataset_create_us",
        "grapher_hdf5_subphase_dataset_write_call_us",
        "grapher_hdf5_subphase_dataset_payload_write_call_us",
        "grapher_hdf5_subphase_dataset_meta_write_call_us",
        "grapher_hdf5_subphase_dataset_object_close_us",
        "grapher_hdf5_subphase_raw_payload_open_us",
        "grapher_hdf5_subphase_raw_payload_preallocate_us",
        "grapher_hdf5_subphase_raw_payload_writev_us",
        "grapher_hdf5_subphase_raw_payload_close_us",
        "grapher_hdf5_subphase_raw_payload_close_wait_us",
        "grapher_hdf5_subphase_raw_payload_bytes",
        "grapher_hdf5_subphase_raw_payload_writev_calls",
        "grapher_hdf5_subphase_raw_payload_partial_writes",
        "grapher_hdf5_subphase_raw_payload_preallocate_result",
        "grapher_hdf5_subphase_flush_us",
        "grapher_hdf5_subphase_file_size_us",
        "grapher_hdf5_subphase_close_us",
        "grapher_hdf5_subphase_rename_us",
        "grapher_hdf5_subphase_publish_rename_us",
        "grapher_hdf5_subphase_archive_manifest_write_us",
        "grapher_hdf5_subphase_writer_total_us",
        "grapher_hdf5_subphase_atomic_rename",
        "grapher_hdf5_subphase_chunk_events",
        "grapher_hdf5_subphase_layout",
        "grapher_async_archive_publish_count",
        "grapher_async_archive_publish_success_count",
        "grapher_async_archive_publish_close_wait_us",
        "grapher_async_archive_publish_close_us",
        "grapher_async_archive_publish_rename_us",
        "grapher_async_archive_publish_manifest_us",
        "grapher_async_archive_publish_total_us",
        "grapher_orphan_chunk_count",
        "grapher_orphan_first_epoch_seconds",
        "grapher_orphan_last_epoch_seconds",
        "release_to_grapher_orphan_first_seconds",
        "release_to_grapher_orphan_last_seconds",
        "release_to_grapher_stop_recording_seconds",
        "release_to_grapher_pipeline_scheduled_seconds",
        "release_to_grapher_pipeline_finalized_seconds",
        "release_to_grapher_hdf5_processing_seconds",
        "release_to_grapher_hdf5_written_seconds",
        "grapher_finalize_to_hdf5_written_seconds",
        "retrieved_event_count",
        "expected_retrieved_event_count",
        "append_count",
        "tail_read_count",
        "tail_success_count",
        "tail_max_retrieved_count",
        "tail_final_retrieved_count",
        "tail_avg_latency_ms",
        "tail_p50_latency_ms",
        "tail_p95_latency_ms",
        "tail_p99_latency_ms",
        "tail_interval_ms",
        "tail_read_mode",
        "tail_reader_start_mode",
        "tail_overlap_ns",
        "keeper_tail_cursor_overlap_ns",
        "tail_final_deadline_seconds",
        "reader_join_timeout_seconds",
        "tail_incremental_retrieved_events",
        "writer_exitcode",
        "reader_exitcode",
        "noise_story_count",
        "noise_events_per_story",
        "live_tail_attempted",
        "live_tail_succeeded",
        "deploy_stop_timeout_seconds",
        "deploy_stop_timed_out",
        "keeper_append_stats_complete",
        "keeper_record_event_count",
        "keeper_journal_append_count",
        "keeper_journal_durable_publish_count",
        "keeper_journal_durable_publish_count_matches_total",
        "keeper_orphan_warning_count",
        "keeper_restart_attempted",
        "keeper_restart_command_success",
        "keeper_restart_health_success",
        "keeper_journal_mode",
        "keeper_journal_dir",
        "keeper_tail_source",
        "keeper_journal_ack_before_ingest",
        "keeper_journal_async_drain",
        "keeper_journal_async_drain_source",
        "keeper_journal_async_drain_threads",
        "keeper_journal_skip_ingest",
        "keeper_journal_defer_rpc_response",
        "keeper_journal_shards",
        "keeper_journal_fdatasync_batch_events",
        "keeper_journal_writev",
        "keeper_journal_batch_writev",
        "keeper_journal_move_batch_payloads",
        "keeper_journal_tail_read_mode",
        "keeper_journal_tail_direct_read_into_events",
        "keeper_tail_batch_max_events",
        "keeper_tail_batch_max_bytes",
        "keeper_journal_atomic_offsets",
        "keeper_journal_single_writer",
        "keeper_journal_single_writer_batch_events",
        "keeper_journal_group_commit_wait",
        "keeper_journal_group_commit_wait_us",
        "keeper_journal_owner_drain_yields",
        "keeper_journal_async_callback_dispatch",
        "keeper_journal_async_batch_completion_dispatch",
        "keeper_journal_callback_dispatch_threads",
        "keeper_wal_drain_batch_events",
        "keeper_journal_file_count",
        "keeper_journal_total_bytes",
        "keeper_journal_record_count",
        "keeper_journal_payload_bytes",
        "keeper_journal_valid",
        "keeper_journal_record_count_matches_total",
        "keeper_append_append_stats_complete",
        "keeper_append_record_event_count_matches_total",
        "keeper_append_keeper_count_expected",
        "keeper_append_keeper_count_with_append_stats",
        "keeper_append_record_event_count",
        "keeper_append_expected_record_event_count",
        "keeper_append_orphan_events",
        "keeper_append_orphan_warning_count",
        "keeper_append_record_event_avg_us",
        "keeper_append_record_event_max_us",
        "keeper_append_record_event_le_100us_count",
        "keeper_append_record_event_le_250us_count",
        "keeper_append_record_event_le_500us_count",
        "keeper_append_record_event_le_1000us_count",
        "keeper_append_record_event_le_2500us_count",
        "keeper_append_record_event_le_5000us_count",
        "keeper_append_record_event_le_10000us_count",
        "keeper_append_record_event_gt_10000us_count",
        "keeper_append_rpc_batch_phase_count",
        "keeper_append_rpc_batch_submit_avg_us",
        "keeper_append_rpc_batch_submit_max_us",
        "keeper_append_rpc_batch_completion_wait_avg_us",
        "keeper_append_rpc_batch_completion_wait_max_us",
        "keeper_append_ingestion_queue_avg_us",
        "keeper_append_ingestion_lookup_avg_us",
        "keeper_append_handle_lock_wait_avg_us",
        "keeper_append_handle_lock_wait_max_us",
        "keeper_append_handle_lock_hold_avg_us",
        "keeper_append_handle_lock_hold_max_us",
        "keeper_append_handle_push_avg_us",
        "keeper_append_handle_collect_count",
        "keeper_append_handle_collect_events",
        "keeper_append_handle_collect_lock_wait_avg_us",
        "keeper_append_handle_collect_lock_wait_max_us",
        "keeper_append_handle_collect_lock_hold_avg_us",
        "keeper_append_handle_collect_lock_hold_max_us",
        "keeper_append_handle_lane_registry_lock_wait_avg_us",
        "keeper_append_handle_lane_registry_lock_wait_max_us",
        "keeper_append_handle_lane_registry_lock_hold_avg_us",
        "keeper_append_handle_lane_registry_lock_hold_max_us",
        "keeper_append_journal_append_count",
        "keeper_append_journal_lock_wait_avg_us",
        "keeper_append_journal_write_avg_us",
        "keeper_append_journal_fdatasync_avg_us",
        "keeper_append_journal_fdatasync_le_100us_count",
        "keeper_append_journal_fdatasync_le_250us_count",
        "keeper_append_journal_fdatasync_le_500us_count",
        "keeper_append_journal_fdatasync_le_1000us_count",
        "keeper_append_journal_fdatasync_le_2500us_count",
        "keeper_append_journal_fdatasync_le_5000us_count",
        "keeper_append_journal_fdatasync_le_10000us_count",
        "keeper_append_journal_fdatasync_gt_10000us_count",
        "keeper_append_journal_publish_avg_us",
        "keeper_append_journal_admission_batch_count",
        "keeper_append_journal_admission_event_count",
        "keeper_append_journal_admission_payload_bytes",
        "keeper_append_journal_admission_avg_events",
        "keeper_append_journal_admission_max_events",
        "keeper_append_journal_admission_avg_shard_groups",
        "keeper_append_journal_admission_max_shard_groups",
        "keeper_append_journal_admission_work_build_avg_us",
        "keeper_append_journal_admission_work_build_max_us",
        "keeper_append_journal_admission_enqueue_avg_us",
        "keeper_append_journal_admission_enqueue_max_us",
        "keeper_append_journal_owner_count",
        "keeper_append_journal_owner_queue_wait_avg_us",
        "keeper_append_journal_owner_queue_wait_max_us",
        "keeper_append_journal_owner_service_avg_us",
        "keeper_append_journal_owner_service_max_us",
        "keeper_append_journal_owner_pending_flush_count",
        "keeper_append_journal_owner_pending_flush_wait_avg_us",
        "keeper_append_journal_owner_pending_flush_wait_max_us",
        "keeper_append_journal_owner_service_le_100us_count",
        "keeper_append_journal_owner_service_le_250us_count",
        "keeper_append_journal_owner_service_le_500us_count",
        "keeper_append_journal_owner_service_le_1000us_count",
        "keeper_append_journal_owner_service_le_2500us_count",
        "keeper_append_journal_owner_service_le_5000us_count",
        "keeper_append_journal_owner_service_le_10000us_count",
        "keeper_append_journal_owner_service_gt_10000us_count",
        "keeper_append_journal_owner_publish_lock_wait_avg_us",
        "keeper_append_journal_owner_publish_lock_wait_max_us",
        "keeper_append_journal_owner_queue_wait_le_100us_count",
        "keeper_append_journal_owner_queue_wait_le_250us_count",
        "keeper_append_journal_owner_queue_wait_le_500us_count",
        "keeper_append_journal_owner_queue_wait_le_1000us_count",
        "keeper_append_journal_owner_queue_wait_le_2500us_count",
        "keeper_append_journal_owner_queue_wait_le_5000us_count",
        "keeper_append_journal_owner_queue_wait_le_10000us_count",
        "keeper_append_journal_owner_queue_wait_gt_10000us_count",
        "keeper_append_journal_owner_batch_count",
        "keeper_append_journal_owner_batch_avg_records",
        "keeper_append_journal_owner_batch_max_records",
        "keeper_append_journal_owner_batch_eq_1_count",
        "keeper_append_journal_owner_batch_le_2_count",
        "keeper_append_journal_owner_batch_le_4_count",
        "keeper_append_journal_owner_batch_le_8_count",
        "keeper_append_journal_owner_batch_le_16_count",
        "keeper_append_journal_owner_batch_le_32_count",
        "keeper_append_journal_owner_batch_le_64_count",
        "keeper_append_journal_owner_batch_gt_64_count",
        "keeper_append_journal_owner_group_commit_count",
        "keeper_append_journal_owner_group_commit_avg_records",
        "keeper_append_journal_owner_group_commit_max_records",
        "keeper_append_journal_owner_group_commit_eq_1_count",
        "keeper_append_journal_owner_group_commit_le_2_count",
        "keeper_append_journal_owner_group_commit_le_4_count",
        "keeper_append_journal_owner_group_commit_le_8_count",
        "keeper_append_journal_owner_group_commit_le_16_count",
        "keeper_append_journal_owner_group_commit_le_32_count",
        "keeper_append_journal_owner_group_commit_le_64_count",
        "keeper_append_journal_owner_group_commit_gt_64_count",
        "keeper_append_journal_owner_queue_enqueue_count",
        "keeper_append_journal_owner_queue_enqueue_avg_records",
        "keeper_append_journal_owner_queue_enqueue_lock_wait_avg_us",
        "keeper_append_journal_owner_queue_enqueue_lock_wait_max_us",
        "keeper_append_journal_owner_queue_enqueue_lock_hold_avg_us",
        "keeper_append_journal_owner_queue_enqueue_lock_hold_max_us",
        "keeper_append_journal_owner_queue_drain_count",
        "keeper_append_journal_owner_queue_drain_avg_records",
        "keeper_append_journal_owner_queue_drain_lock_wait_avg_us",
        "keeper_append_journal_owner_queue_drain_lock_wait_max_us",
        "keeper_append_journal_owner_queue_drain_lock_hold_avg_us",
        "keeper_append_journal_owner_queue_drain_lock_hold_max_us",
        "keeper_append_journal_owner_queue_depth_max",
        "keeper_append_journal_group_sync_count",
        "keeper_append_journal_group_sync_records",
        "keeper_append_journal_group_sync_avg_us",
        "keeper_append_journal_group_sync_max_us",
        "keeper_append_journal_group_sync_avg_records",
        "keeper_append_journal_group_sync_max_records",
        "keeper_append_journal_group_sync_le_100us_count",
        "keeper_append_journal_group_sync_le_250us_count",
        "keeper_append_journal_group_sync_le_500us_count",
        "keeper_append_journal_group_sync_le_1000us_count",
        "keeper_append_journal_group_sync_le_2500us_count",
        "keeper_append_journal_group_sync_le_5000us_count",
        "keeper_append_journal_group_sync_le_10000us_count",
        "keeper_append_journal_group_sync_gt_10000us_count",
        "keeper_append_journal_group_sync_eq_1_count",
        "keeper_append_journal_group_sync_le_2_count",
        "keeper_append_journal_group_sync_le_4_count",
        "keeper_append_journal_group_sync_le_8_count",
        "keeper_append_journal_group_sync_le_16_count",
        "keeper_append_journal_group_sync_le_32_count",
        "keeper_append_journal_group_sync_le_64_count",
        "keeper_append_journal_group_sync_gt_64_count",
        "keeper_append_journal_durable_publish_count",
        "keeper_append_journal_durable_publish_batch_count",
        "keeper_append_journal_durable_publish_batch_avg_records",
        "keeper_append_journal_durable_publish_batch_max_records",
        "keeper_append_journal_durable_publish_batch_eq_1_count",
        "keeper_append_journal_durable_publish_batch_le_2_count",
        "keeper_append_journal_durable_publish_batch_le_4_count",
        "keeper_append_journal_durable_publish_batch_le_8_count",
        "keeper_append_journal_durable_publish_batch_le_16_count",
        "keeper_append_journal_durable_publish_batch_le_32_count",
        "keeper_append_journal_durable_publish_batch_le_64_count",
        "keeper_append_journal_durable_publish_batch_gt_64_count",
        "keeper_append_journal_callback_dispatch_enqueue_count",
        "keeper_append_journal_callback_dispatch_enqueue_lock_wait_avg_us",
        "keeper_append_journal_callback_dispatch_enqueue_lock_wait_max_us",
        "keeper_append_journal_callback_dispatch_queue_depth_max",
        "keeper_append_journal_callback_dispatch_count",
        "keeper_append_journal_callback_dispatch_queue_wait_avg_us",
        "keeper_append_journal_callback_dispatch_queue_wait_max_us",
        "keeper_append_journal_callback_dispatch_service_avg_us",
        "keeper_append_journal_callback_dispatch_service_max_us",
        "keeper_append_journal_callback_dispatch_queue_wait_le_100us_count",
        "keeper_append_journal_callback_dispatch_queue_wait_le_250us_count",
        "keeper_append_journal_callback_dispatch_queue_wait_le_500us_count",
        "keeper_append_journal_callback_dispatch_queue_wait_le_1000us_count",
        "keeper_append_journal_callback_dispatch_queue_wait_le_2500us_count",
        "keeper_append_journal_callback_dispatch_queue_wait_le_5000us_count",
        "keeper_append_journal_callback_dispatch_queue_wait_le_10000us_count",
        "keeper_append_journal_callback_dispatch_queue_wait_gt_10000us_count",
        "keeper_append_journal_tail_read_count",
        "keeper_append_journal_tail_snapshot_avg_us",
        "keeper_append_journal_tail_snapshot_lock_wait_avg_us",
        "keeper_append_journal_tail_payload_read_avg_us",
        "keeper_append_journal_tail_payload_read_le_100us_count",
        "keeper_append_journal_tail_payload_read_le_250us_count",
        "keeper_append_journal_tail_payload_read_le_500us_count",
        "keeper_append_journal_tail_payload_read_le_1000us_count",
        "keeper_append_journal_tail_payload_read_le_2500us_count",
        "keeper_append_journal_tail_payload_read_le_5000us_count",
        "keeper_append_journal_tail_payload_read_le_10000us_count",
        "keeper_append_journal_tail_payload_read_gt_10000us_count",
        "keeper_append_journal_tail_read_group_count",
        "keeper_append_journal_tail_read_group_avg",
        "keeper_append_journal_tail_read_group_max",
        "keeper_append_journal_tail_physical_read_bytes",
        "keeper_append_journal_tail_payload_copy_bytes",
        "keeper_append_journal_tail_overread_bytes",
        "keeper_append_journal_tail_direct_read_count",
        "keeper_append_journal_tail_coalesced_read_count",
        "keeper_append_journal_tail_vectored_read_count",
        "keeper_append_post_ack_ingest_count",
        "keeper_append_post_ack_ingest_avg_us",
        "keeper_append_post_ack_ingest_max_us",
        "keeper_append_async_drain_enqueue_count",
        "keeper_append_async_drain_complete_count",
        "keeper_append_async_drain_avg_us",
        "keeper_append_async_drain_max_us",
        "keeper_append_async_drain_queue_depth_max",
        "keeper_append_wal_drain_read_count",
        "keeper_append_wal_drain_read_payload_bytes",
        "keeper_append_wal_drain_read_avg_us",
        "keeper_append_wal_drain_read_max_us",
        "keeper_append_wal_drain_read_batch_count",
        "keeper_append_wal_drain_read_batch_avg_records",
        "keeper_append_wal_drain_read_batch_max_records",
        "keeper_append_wal_drain_read_physical_bytes",
        "keeper_append_wal_drain_read_gap_bytes",
        "keeper_append_wal_drain_read_syscall_count",
        "keeper_append_wal_drain_read_syscall_avg_records",
        "keeper_append_wal_drain_batch_wait_count",
        "keeper_append_wal_drain_batch_wait_avg_us",
        "keeper_append_wal_drain_batch_wait_max_us",
        "keeper_append_timeline_merge_batch_count",
        "keeper_append_timeline_merge_event_count",
        "keeper_append_timeline_merge_inserted_count",
        "keeper_append_timeline_merge_batch_avg_events",
        "keeper_append_timeline_merge_batch_max_events",
        "keeper_append_timeline_merge_lock_wait_avg_us",
        "keeper_append_timeline_merge_lock_wait_max_us",
        "keeper_append_timeline_merge_lock_hold_avg_us",
        "keeper_append_timeline_merge_lock_hold_max_us",
        "keeper_append_timeline_merge_insert_avg_us",
        "keeper_append_timeline_merge_insert_max_us",
        "keeper_append_handle_queue_depth_max",
        "client_append_client_count_with_stats",
        "client_append_submit_call_count",
        "client_append_submit_event_count",
        "client_append_expected_event_count",
        "client_append_submit_event_count_matches_total",
        "client_append_submit_payload_bytes",
        "client_append_submit_avg_us",
        "client_append_submit_max_us",
        "client_append_future_count",
        "client_append_future_count_avg_per_submit",
        "client_append_future_count_max_per_submit",
        "client_append_future_wait_count",
        "client_append_future_wait_avg_us",
        "client_append_future_wait_max_us",
        "success",
        "error",
        "metrics_path",
        "result_dir",
    ]
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=keys, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def write_skipped_unsupported_client_count(rows: list[dict[str, Any]], path: Path) -> None:
    keys = [
        "system",
        "workflow",
        "trial",
        "node_count",
        "client_count",
        "message_size_bytes",
        "operation_count",
        "reason",
    ]
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=keys, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    systems = csv_strings(args.systems)
    workflows = csv_strings(args.workflows)
    node_counts = csv_ints(args.node_counts)
    message_sizes = csv_ints(args.message_sizes)
    operation_counts = csv_ints(args.operation_counts)
    client_counts = csv_ints(args.client_counts)
    mofka_partition_types = csv_strings(args.mofka_partition_types)
    mofka_storage_target_types = csv_strings(args.mofka_storage_target_types)
    mofka_storage_target_sizes = csv_ints(args.mofka_storage_target_sizes)
    mofka_producer_wait_modes = csv_strings(args.mofka_producer_wait_modes)
    mofka_producer_flush_modes = csv_strings(args.mofka_producer_flush_modes)
    mofka_precreate_storage_provider_modes = csv_strings(args.mofka_precreate_storage_provider_values)
    kafka_acks_values = csv_strings(args.kafka_acks_values)
    chronolog_completion_modes = csv_strings(args.chronolog_completion_modes)
    chronolog_data_collection_poll_intervals_us = csv_ints(args.chronolog_data_collection_poll_intervals_us)
    chronolog_archive_event_count_poll_intervals_seconds = csv_strings(
        args.chronolog_archive_event_count_poll_intervals_seconds
    )
    chronolog_archive_readback_mode_values = csv_strings(args.chronolog_archive_readback_modes)
    chronolog_keeper_data_collection_stream_values = csv_ints(args.chronolog_keeper_data_collection_streams)
    chronolog_keeper_data_collection_threads_per_stream_values = csv_ints(
        args.chronolog_keeper_data_collection_threads_per_stream
    )
    chronolog_grapher_data_collection_stream_values = csv_ints(args.chronolog_grapher_data_collection_streams)
    chronolog_grapher_data_collection_threads_per_stream_values = csv_ints(
        args.chronolog_grapher_data_collection_threads_per_stream
    )
    if args.chronolog_grapher_inactive_story_delay_seconds == "":
        chronolog_grapher_inactive_story_delay_values = [""]
    else:
        chronolog_grapher_inactive_story_delay_values = csv_ints(
            args.chronolog_grapher_inactive_story_delay_seconds
        )
    chronolog_chrono_bench_barrier_modes = csv_strings(args.chronolog_chrono_bench_barrier_modes)
    chronolog_chrono_bench_shared_story_modes = csv_strings(args.chronolog_chrono_bench_shared_story_modes)
    chronolog_producer_outstanding_values = csv_ints(args.chronolog_producer_outstanding_values)
    chronolog_producer_batch_sizes = csv_ints(args.chronolog_producer_batch_sizes)
    chronolog_producer_wait_policies = csv_strings(args.chronolog_producer_wait_modes)
    for chronolog_producer_wait_policy in chronolog_producer_wait_policies:
        chronolog_producer_wait_label(chronolog_producer_wait_policy, 1, 1)
    chronolog_client_batch_keeper_selection_values = csv_strings(
        args.chronolog_client_batch_keeper_selection_values
    )
    chronolog_client_keeper_time_bucket_ns_values = csv_ints(
        args.chronolog_client_keeper_time_bucket_ns_values
    )
    chronolog_client_parallel_tail_rpc_values = csv_strings(args.chronolog_client_parallel_tail_rpc_values)
    chronolog_client_keeper_cursor_drain_values = csv_strings(args.chronolog_client_keeper_cursor_drain_values)
    chronolog_client_keeper_cursor_drain_max_batches_values = csv_ints(
        args.chronolog_client_keeper_cursor_drain_max_batches_values
    )
    chronolog_client_keeper_cursor_packed_batch_values = csv_strings(
        args.chronolog_client_keeper_cursor_packed_batch_values
    )
    chronolog_client_keeper_cursor_packed_bulk_values = csv_strings(
        args.chronolog_client_keeper_cursor_packed_bulk_values
    )
    chronolog_client_keeper_cursor_packed_bulk_stream_values = csv_strings(
        args.chronolog_client_keeper_cursor_packed_bulk_stream_values
    )
    chronolog_client_keeper_cursor_packed_bulk_stream_max_batches_values = csv_ints(
        args.chronolog_client_keeper_cursor_packed_bulk_stream_max_batches_values
    )
    chronolog_client_keeper_cursor_packed_bulk_buffer_bytes_values = csv_ints(
        args.chronolog_client_keeper_cursor_packed_bulk_buffer_bytes_values
    )
    chronolog_mixed_tail_read_mode_values = csv_strings(args.chronolog_mixed_tail_read_modes)
    chronolog_mixed_tail_reader_start_mode_values = csv_strings(args.chronolog_mixed_tail_reader_start_modes)
    chronolog_client_execution_modes = parse_chronolog_client_execution_modes(args.chronolog_client_execution_modes)
    chronolog_grapher_retire_on_stop_values = csv_strings(args.chronolog_grapher_retire_on_stop_values)
    chronolog_grapher_stop_retire_grace_us_values = csv_ints(
        args.chronolog_grapher_stop_retire_grace_us_values
    )
    chronolog_grapher_stop_story_archive_drain_values = csv_ints(
        args.chronolog_grapher_stop_story_archive_drain_values
    )
    chronolog_grapher_stop_story_archive_drain_timeout_ms_values = csv_ints(
        args.chronolog_grapher_stop_story_archive_drain_timeout_ms
    )
    chronolog_grapher_extraction_threads_values = csv_ints(args.chronolog_grapher_extraction_threads)
    chronolog_hdf5_archive_atomic_rename_values = csv_strings(args.chronolog_hdf5_archive_atomic_rename_values)
    chronolog_hdf5_archive_chunk_events_values = csv_ints(args.chronolog_hdf5_archive_chunk_events)
    chronolog_hdf5_archive_layout_values = csv_strings(args.chronolog_hdf5_archive_layouts)
    chronolog_raw_blob_preallocate_values = csv_strings(args.chronolog_raw_blob_preallocate_values)
    chronolog_raw_blob_async_close_values = csv_strings(args.chronolog_raw_blob_async_close_values)
    chronolog_raw_blob_async_publish_values = csv_strings(args.chronolog_raw_blob_async_publish_values)
    chronolog_raw_blob_async_publish_threads_values = csv_ints(args.chronolog_raw_blob_async_publish_threads)
    chronolog_archive_range_event_count_values = csv_ints(args.chronolog_archive_range_event_counts)
    chronolog_keeper_journal_placements = csv_strings(args.chronolog_keeper_journal_placements)
    chronolog_keeper_journal_batch_writev_values = csv_strings(
        args.chronolog_keeper_journal_batch_writev_values
    )
    chronolog_keeper_journal_move_batch_payloads_values = csv_strings(
        args.chronolog_keeper_journal_move_batch_payloads_values
    )
    chronolog_keeper_tail_batch_max_event_values = csv_ints(args.chronolog_keeper_tail_batch_max_events)
    chronolog_keeper_tail_batch_max_byte_values = csv_ints(args.chronolog_keeper_tail_batch_max_bytes)
    chronolog_keeper_journal_group_commit_flush_bytes_values = csv_ints(
        args.chronolog_keeper_journal_group_commit_flush_bytes
    )
    chronolog_keeper_journal_group_commit_strict_flush_event_cap_values = csv_ints(
        args.chronolog_keeper_journal_group_commit_strict_flush_event_cap_values
    )
    chronolog_keeper_journal_group_commit_large_payload_bytes_values = csv_ints(
        args.chronolog_keeper_journal_group_commit_large_payload_bytes
    )
    chronolog_keeper_journal_group_commit_large_payload_flush_events_values = csv_ints(
        args.chronolog_keeper_journal_group_commit_large_payload_flush_events
    )
    chronolog_keeper_journal_owner_drain_yields_values = csv_ints(
        args.chronolog_keeper_journal_owner_drain_yields
    )
    chronolog_keeper_journal_notify_owner_only_on_empty_values = csv_ints(
        args.chronolog_keeper_journal_notify_owner_only_on_empty_values
    )
    chronolog_keeper_journal_async_callback_dispatch_values = csv_ints(
        args.chronolog_keeper_journal_async_callback_dispatch_values
    )
    chronolog_keeper_journal_async_batch_completion_dispatch_values = csv_ints(
        args.chronolog_keeper_journal_async_batch_completion_dispatch_values
    )
    chronolog_keeper_journal_callback_dispatch_threads = csv_ints(
        args.chronolog_keeper_journal_callback_dispatch_threads
    )
    chronolog_keeper_journal_callback_batch_drain_values = csv_ints(
        args.chronolog_keeper_journal_callback_batch_drain_values
    )
    chronolog_keeper_journal_callback_batch_drain_max_values = csv_ints(
        args.chronolog_keeper_journal_callback_batch_drain_max_values
    )
    chronolog_keeper_journal_callback_batch_drain_min_payload_bytes_values = csv_ints(
        args.chronolog_keeper_journal_callback_batch_drain_min_payload_bytes_values
    )
    chronolog_keeper_journal_durable_complete_before_publish_values = csv_ints(
        args.chronolog_keeper_journal_durable_complete_before_publish_values
    )
    chronolog_keeper_append_stats_interval_events_values = csv_ints(
        args.chronolog_keeper_append_stats_interval_events
    )
    chronolog_keeper_recording_margo_xstreams = csv_ints(args.chronolog_keeper_recording_margo_xstreams)
    chronolog_keeper_recording_margo_progress_thread_values = csv_ints(
        args.chronolog_keeper_recording_margo_progress_thread_values
    )
    chronolog_keeper_recording_margo_handlers = csv_ints(args.chronolog_keeper_recording_margo_handlers)
    chronolog_grapher_recording_margo_xstreams = csv_ints(args.chronolog_grapher_recording_margo_xstreams)
    chronolog_grapher_recording_margo_handlers = csv_ints(args.chronolog_grapher_recording_margo_handlers)
    chronolog_grapher_direct_deserialize_values = csv_ints(args.chronolog_grapher_direct_deserialize_values)
    chronolog_keeper_drain_margo_progress_thread_values = csv_ints(
        args.chronolog_keeper_drain_margo_progress_thread_values
    )
    chronolog_keeper_drain_margo_rpc_thread_values = csv_ints(args.chronolog_keeper_drain_margo_rpc_threads)
    chronolog_keeper_extraction_thread_values = csv_ints(args.chronolog_keeper_extraction_threads)
    chronolog_keeper_direct_serialize_values = csv_ints(args.chronolog_keeper_direct_serialize_values)
    chronolog_keeper_fast_wire_values = csv_ints(args.chronolog_keeper_fast_wire_values)
    chronolog_keeper_stop_story_flush_drain_values = csv_ints(args.chronolog_keeper_stop_story_flush_drain_values)
    chronolog_keeper_stop_story_flush_drain_timeout_ms_values = csv_ints(
        args.chronolog_keeper_stop_story_flush_drain_timeout_ms
    )
    chronolog_visor_parallel_keeper_stop_values = csv_ints(args.chronolog_visor_parallel_keeper_stop_values)
    root = run_dir(args)
    matrix_dir = root / "benchmark-matrix"
    matrix_dir.mkdir(parents=True, exist_ok=True)

    runs: list[dict[str, Any]] = []
    skipped_unsupported_client_count: list[dict[str, Any]] = []
    for system, workflow, node_count, message_size, op_count, client_count, trial in itertools.product(
        systems, workflows, node_counts, message_sizes, operation_counts, client_counts, range(1, args.trials + 1)
    ):
        if workflow not in WORKFLOW_BY_SYSTEM or system not in WORKFLOW_BY_SYSTEM[workflow]:
            continue
        if system == "chronolog" and not chronolog_client_count_supported(workflow, client_count):
            skipped_unsupported_client_count.append(
                {
                    "system": system,
                    "workflow": workflow,
                    "trial": trial,
                    "node_count": node_count,
                    "message_size_bytes": message_size,
                    "operation_count": op_count,
                    "client_count": client_count,
                    "reason": (
                        "ChronoLog workflow does not have a validated real multi-client execution path; "
                        "supported c>1 workflows are "
                        + ",".join(sorted(CHRONOLOG_MULTI_CLIENT_WORKFLOWS))
                    ),
                }
            )
            continue
        backend_pairs = [("", "")]
        completion_modes = [""]
        chronolog_poll_intervals_us = [""]
        chronolog_keeper_data_collection_stream_modes = [""]
        chronolog_keeper_data_collection_threads_per_stream_modes = [""]
        chronolog_grapher_data_collection_stream_modes = [""]
        chronolog_grapher_data_collection_threads_per_stream_modes = [""]
        chronolog_grapher_inactive_story_delay_modes = [""]
        chronolog_barrier_modes = [""]
        chronolog_shared_story_modes = [""]
        chronolog_producer_outstanding_modes = [""]
        chronolog_producer_batch_size_modes = [""]
        chronolog_producer_wait_policy_modes = [""]
        chronolog_client_batch_keeper_selection_modes = [""]
        chronolog_client_keeper_time_bucket_ns_modes = [""]
        chronolog_client_parallel_tail_rpc_modes = [""]
        chronolog_client_keeper_cursor_drain_modes = [""]
        chronolog_client_keeper_cursor_drain_max_batch_modes = [""]
        chronolog_client_keeper_cursor_packed_batch_modes = [""]
        chronolog_client_keeper_cursor_packed_bulk_modes = [""]
        chronolog_client_keeper_cursor_packed_bulk_stream_modes = [""]
        chronolog_client_keeper_cursor_packed_bulk_stream_max_batch_modes = [""]
        chronolog_client_keeper_cursor_packed_bulk_buffer_byte_modes = [""]
        chronolog_mixed_tail_read_mode_modes = [""]
        chronolog_mixed_tail_reader_start_mode_modes = [""]
        chronolog_client_execution_mode_values = [""]
        chronolog_grapher_retire_on_stop_modes = [""]
        chronolog_grapher_stop_retire_grace_us_modes = [""]
        chronolog_grapher_stop_story_archive_drain_modes = [""]
        chronolog_grapher_stop_story_archive_drain_timeout_ms_modes = [""]
        chronolog_grapher_extraction_thread_modes = [""]
        chronolog_hdf5_archive_atomic_rename_modes = [""]
        chronolog_hdf5_archive_chunk_event_modes = [""]
        chronolog_hdf5_archive_layout_modes = [""]
        chronolog_raw_blob_preallocate_modes = [""]
        chronolog_archive_event_count_poll_interval_modes = [""]
        chronolog_archive_readback_modes = [""]
        chronolog_archive_range_event_count_modes = [""]
        chronolog_keeper_journal_shards = [""]
        chronolog_keeper_journal_shard_policies = [""]
        chronolog_keeper_journal_placement_modes = [""]
        chronolog_keeper_journal_fdatasync_batch_events = [""]
        chronolog_keeper_journal_batch_writev_modes = [""]
        chronolog_keeper_journal_move_batch_payloads_modes = [""]
        chronolog_keeper_tail_batch_max_events = [""]
        chronolog_keeper_tail_batch_max_bytes = [""]
        chronolog_keeper_journal_group_commit_flush_events = [""]
        chronolog_keeper_journal_group_commit_strict_flush_event_cap = [""]
        chronolog_keeper_journal_group_commit_flush_bytes = [""]
        chronolog_keeper_journal_group_commit_large_payload_bytes = [""]
        chronolog_keeper_journal_group_commit_large_payload_flush_events = [""]
        chronolog_keeper_journal_group_commit_wait_us = [""]
        chronolog_keeper_journal_group_commit_flush_wait_us = [""]
        chronolog_keeper_journal_owner_drain_yields = [""]
        chronolog_keeper_journal_notify_owner_only_on_empty = [""]
        chronolog_keeper_append_stats_interval_events = [""]
        chronolog_keeper_wal_drain_batch_events = [""]
        chronolog_keeper_wal_drain_batch_wait_us = [""]
        chronolog_keeper_journal_async_drain_thread_modes = [""]
        chronolog_keeper_journal_async_callback_dispatch_modes = [""]
        chronolog_keeper_journal_async_batch_completion_dispatch_modes = [""]
        chronolog_keeper_journal_callback_dispatch_thread_modes = [""]
        chronolog_keeper_journal_callback_batch_drain_modes = [""]
        chronolog_keeper_journal_callback_batch_drain_max_modes = [""]
        chronolog_keeper_journal_callback_batch_drain_min_payload_bytes_modes = [""]
        chronolog_keeper_journal_durable_complete_before_publish_modes = [""]
        chronolog_keeper_recording_margo_xstream_modes = [""]
        chronolog_keeper_recording_margo_progress_thread_modes = [""]
        chronolog_keeper_recording_margo_handler_modes = [""]
        chronolog_grapher_recording_margo_xstream_modes = [""]
        chronolog_grapher_recording_margo_handler_modes = [""]
        chronolog_grapher_direct_deserialize_modes = [""]
        chronolog_keeper_drain_margo_progress_thread_modes = [""]
        chronolog_keeper_drain_margo_rpc_thread_modes = [""]
        chronolog_keeper_extraction_thread_modes = [""]
        chronolog_keeper_direct_serialize_modes = [""]
        chronolog_keeper_fast_wire_modes = [""]
        chronolog_keeper_stop_story_flush_drain_modes = [""]
        chronolog_keeper_stop_story_flush_drain_timeout_ms_modes = [""]
        chronolog_visor_parallel_keeper_stop_modes = [""]
        kafka_acks_modes = [""]
        mofka_storage_sizes_for_system = [""]
        if system == "chronolog":
            completion_modes = chronolog_completion_modes
            chronolog_poll_intervals_us = chronolog_data_collection_poll_intervals_us
            chronolog_archive_event_count_poll_interval_modes = (
                chronolog_archive_event_count_poll_intervals_seconds
            )
            chronolog_archive_readback_modes = (
                chronolog_archive_readback_mode_values if workflow == "archive_range_retrieval" else [""]
            )
            chronolog_keeper_data_collection_stream_modes = chronolog_keeper_data_collection_stream_values
            chronolog_keeper_data_collection_threads_per_stream_modes = (
                chronolog_keeper_data_collection_threads_per_stream_values
            )
            chronolog_grapher_data_collection_stream_modes = chronolog_grapher_data_collection_stream_values
            chronolog_grapher_data_collection_threads_per_stream_modes = (
                chronolog_grapher_data_collection_threads_per_stream_values
            )
            chronolog_grapher_inactive_story_delay_modes = chronolog_grapher_inactive_story_delay_values
            chronolog_barrier_modes = chronolog_chrono_bench_barrier_modes
            chronolog_shared_story_modes = chronolog_chrono_bench_shared_story_modes
            chronolog_producer_outstanding_modes = chronolog_producer_outstanding_values
            chronolog_producer_batch_size_modes = chronolog_producer_batch_sizes
            chronolog_producer_wait_policy_modes = chronolog_producer_wait_policies
            chronolog_client_batch_keeper_selection_modes = chronolog_client_batch_keeper_selection_values
            chronolog_client_keeper_time_bucket_ns_modes = chronolog_client_keeper_time_bucket_ns_values
            chronolog_client_parallel_tail_rpc_modes = chronolog_client_parallel_tail_rpc_values
            chronolog_client_keeper_cursor_drain_modes = (
                chronolog_client_keeper_cursor_drain_values if workflow == "mixed_append_tail" else ["0"]
            )
            chronolog_client_keeper_cursor_drain_max_batch_modes = (
                chronolog_client_keeper_cursor_drain_max_batches_values
                if workflow == "mixed_append_tail"
                else ["0"]
            )
            chronolog_client_keeper_cursor_packed_batch_modes = (
                chronolog_client_keeper_cursor_packed_batch_values if workflow == "mixed_append_tail" else ["0"]
            )
            chronolog_client_keeper_cursor_packed_bulk_modes = (
                chronolog_client_keeper_cursor_packed_bulk_values if workflow == "mixed_append_tail" else ["0"]
            )
            chronolog_client_keeper_cursor_packed_bulk_stream_modes = (
                chronolog_client_keeper_cursor_packed_bulk_stream_values
                if workflow == "mixed_append_tail"
                else ["0"]
            )
            chronolog_client_keeper_cursor_packed_bulk_stream_max_batch_modes = (
                chronolog_client_keeper_cursor_packed_bulk_stream_max_batches_values
                if workflow == "mixed_append_tail"
                else [0]
            )
            chronolog_client_keeper_cursor_packed_bulk_buffer_byte_modes = (
                chronolog_client_keeper_cursor_packed_bulk_buffer_bytes_values
                if workflow == "mixed_append_tail"
                else [0]
            )
            chronolog_mixed_tail_read_mode_modes = (
                chronolog_mixed_tail_read_mode_values if workflow == "mixed_append_tail" else ["full"]
            )
            chronolog_mixed_tail_reader_start_mode_modes = (
                chronolog_mixed_tail_reader_start_mode_values if workflow == "mixed_append_tail" else ["ready"]
            )
            chronolog_client_execution_mode_values = chronolog_client_execution_modes
            chronolog_grapher_retire_on_stop_modes = chronolog_grapher_retire_on_stop_values
            chronolog_grapher_stop_retire_grace_us_modes = chronolog_grapher_stop_retire_grace_us_values
            chronolog_grapher_stop_story_archive_drain_modes = chronolog_grapher_stop_story_archive_drain_values
            chronolog_grapher_stop_story_archive_drain_timeout_ms_modes = (
                chronolog_grapher_stop_story_archive_drain_timeout_ms_values
            )
            chronolog_grapher_extraction_thread_modes = chronolog_grapher_extraction_threads_values
            chronolog_hdf5_archive_atomic_rename_modes = chronolog_hdf5_archive_atomic_rename_values
            chronolog_hdf5_archive_chunk_event_modes = chronolog_hdf5_archive_chunk_events_values
            chronolog_hdf5_archive_layout_modes = chronolog_hdf5_archive_layout_values
            chronolog_raw_blob_preallocate_modes = chronolog_raw_blob_preallocate_values
            chronolog_archive_range_event_count_modes = (
                chronolog_archive_range_event_count_values
                if workflow == "archive_range_retrieval"
                else [""]
            )
            chronolog_keeper_journal_shards = csv_ints(args.chronolog_keeper_journal_shards)
            chronolog_keeper_journal_shard_policies = csv_strings(args.chronolog_keeper_journal_shard_policies)
            chronolog_keeper_journal_placement_modes = chronolog_keeper_journal_placements
            chronolog_keeper_journal_fdatasync_batch_events = csv_ints(
                args.chronolog_keeper_journal_fdatasync_batch_events
            )
            chronolog_keeper_journal_batch_writev_modes = chronolog_keeper_journal_batch_writev_values
            chronolog_keeper_journal_move_batch_payloads_modes = (
                chronolog_keeper_journal_move_batch_payloads_values
            )
            chronolog_keeper_tail_batch_max_events = chronolog_keeper_tail_batch_max_event_values
            chronolog_keeper_tail_batch_max_bytes = chronolog_keeper_tail_batch_max_byte_values
            chronolog_keeper_journal_group_commit_flush_events = csv_ints(
                args.chronolog_keeper_journal_group_commit_flush_events
            )
            chronolog_keeper_journal_group_commit_strict_flush_event_cap = (
                chronolog_keeper_journal_group_commit_strict_flush_event_cap_values
            )
            chronolog_keeper_journal_group_commit_flush_bytes = chronolog_keeper_journal_group_commit_flush_bytes_values
            chronolog_keeper_journal_group_commit_large_payload_bytes = (
                chronolog_keeper_journal_group_commit_large_payload_bytes_values
            )
            chronolog_keeper_journal_group_commit_large_payload_flush_events = (
                chronolog_keeper_journal_group_commit_large_payload_flush_events_values
            )
            chronolog_keeper_journal_group_commit_wait_us = csv_ints(
                args.chronolog_keeper_journal_group_commit_wait_us
            )
            chronolog_keeper_journal_group_commit_flush_wait_us = csv_ints(
                args.chronolog_keeper_journal_group_commit_flush_wait_us
            )
            chronolog_keeper_journal_owner_drain_yields = chronolog_keeper_journal_owner_drain_yields_values
            chronolog_keeper_journal_notify_owner_only_on_empty = (
                chronolog_keeper_journal_notify_owner_only_on_empty_values
            )
            chronolog_keeper_append_stats_interval_events = chronolog_keeper_append_stats_interval_events_values
            chronolog_keeper_wal_drain_batch_events = csv_ints(args.chronolog_keeper_wal_drain_batch_events)
            chronolog_keeper_wal_drain_batch_wait_us = csv_ints(args.chronolog_keeper_wal_drain_batch_wait_us)
            chronolog_keeper_journal_async_drain_thread_modes = csv_ints(
                args.chronolog_keeper_journal_async_drain_threads
            )
            chronolog_keeper_journal_async_callback_dispatch_modes = (
                chronolog_keeper_journal_async_callback_dispatch_values
            )
            chronolog_keeper_journal_async_batch_completion_dispatch_modes = (
                chronolog_keeper_journal_async_batch_completion_dispatch_values
            )
            chronolog_keeper_journal_callback_dispatch_thread_modes = (
                chronolog_keeper_journal_callback_dispatch_threads
            )
            chronolog_keeper_journal_callback_batch_drain_modes = (
                chronolog_keeper_journal_callback_batch_drain_values
            )
            chronolog_keeper_journal_callback_batch_drain_max_modes = (
                chronolog_keeper_journal_callback_batch_drain_max_values
            )
            chronolog_keeper_journal_callback_batch_drain_min_payload_bytes_modes = (
                chronolog_keeper_journal_callback_batch_drain_min_payload_bytes_values
            )
            chronolog_keeper_journal_durable_complete_before_publish_modes = (
                chronolog_keeper_journal_durable_complete_before_publish_values
            )
            chronolog_keeper_recording_margo_xstream_modes = chronolog_keeper_recording_margo_xstreams
            chronolog_keeper_recording_margo_progress_thread_modes = (
                chronolog_keeper_recording_margo_progress_thread_values
            )
            chronolog_keeper_recording_margo_handler_modes = chronolog_keeper_recording_margo_handlers
            chronolog_grapher_recording_margo_xstream_modes = chronolog_grapher_recording_margo_xstreams
            chronolog_grapher_recording_margo_handler_modes = chronolog_grapher_recording_margo_handlers
            chronolog_grapher_direct_deserialize_modes = chronolog_grapher_direct_deserialize_values
            chronolog_keeper_drain_margo_progress_thread_modes = chronolog_keeper_drain_margo_progress_thread_values
            chronolog_keeper_drain_margo_rpc_thread_modes = chronolog_keeper_drain_margo_rpc_thread_values
            chronolog_keeper_extraction_thread_modes = chronolog_keeper_extraction_thread_values
            chronolog_keeper_direct_serialize_modes = chronolog_keeper_direct_serialize_values
            chronolog_keeper_fast_wire_modes = chronolog_keeper_fast_wire_values
            chronolog_keeper_stop_story_flush_drain_modes = chronolog_keeper_stop_story_flush_drain_values
            chronolog_keeper_stop_story_flush_drain_timeout_ms_modes = (
                chronolog_keeper_stop_story_flush_drain_timeout_ms_values
            )
            chronolog_visor_parallel_keeper_stop_modes = chronolog_visor_parallel_keeper_stop_values
        if system == "mofka":
            backend_pairs = mofka_backend_modes(
                mofka_partition_types,
                mofka_storage_target_types,
                mofka_producer_wait_modes,
                mofka_producer_flush_modes,
                mofka_precreate_storage_provider_modes,
            )
            mofka_storage_sizes_for_system = mofka_storage_target_sizes
        if system == "kafka":
            kafka_acks_modes = kafka_acks_values
        for (
            backend_pair,
            mofka_storage_target_size,
            kafka_acks,
            chronolog_completion_mode,
            chronolog_poll_interval_us,
            chronolog_keeper_data_collection_stream_count,
            chronolog_keeper_data_collection_threads_per_stream_count,
            chronolog_grapher_data_collection_stream_count,
            chronolog_grapher_data_collection_threads_per_stream_count,
            chronolog_grapher_inactive_story_delay_seconds,
            chronolog_barrier_mode,
            chronolog_shared_story_mode,
            chronolog_producer_outstanding,
            chronolog_producer_batch_size,
            chronolog_producer_wait_policy,
            chronolog_client_batch_keeper_selection,
            chronolog_client_keeper_time_bucket_ns,
            chronolog_client_parallel_tail_rpc,
            chronolog_client_keeper_cursor_drain,
            chronolog_client_keeper_cursor_drain_max_batches,
            chronolog_client_keeper_cursor_packed_batch,
            chronolog_client_keeper_cursor_packed_bulk,
            chronolog_client_keeper_cursor_packed_bulk_stream,
            chronolog_client_keeper_cursor_packed_bulk_stream_max_batches,
            chronolog_client_keeper_cursor_packed_bulk_buffer_bytes,
            chronolog_mixed_tail_read_mode,
            chronolog_mixed_tail_reader_start_mode,
            chronolog_client_execution_mode,
            chronolog_grapher_retire_on_stop,
            chronolog_grapher_stop_retire_grace_us,
            chronolog_grapher_stop_story_archive_drain,
            chronolog_grapher_stop_story_archive_drain_timeout_ms,
            chronolog_grapher_extraction_threads,
            chronolog_hdf5_archive_atomic_rename,
            chronolog_hdf5_archive_chunk_events,
            chronolog_hdf5_archive_layout,
            chronolog_raw_blob_preallocate,
            chronolog_raw_blob_async_close,
            chronolog_raw_blob_async_publish,
            chronolog_raw_blob_async_publish_threads,
            chronolog_archive_event_count_poll_interval_seconds,
            chronolog_archive_readback_mode,
            chronolog_archive_range_event_count,
            chronolog_keeper_journal_shard_count,
            chronolog_keeper_journal_shard_policy,
            chronolog_keeper_journal_placement,
            chronolog_keeper_journal_fdatasync_batch_event_count,
            chronolog_keeper_journal_batch_writev,
            chronolog_keeper_journal_move_batch_payloads,
            chronolog_keeper_tail_batch_max_event_count,
            chronolog_keeper_tail_batch_max_byte_count,
            chronolog_keeper_journal_group_commit_flush_event_count,
            chronolog_keeper_journal_group_commit_strict_flush_event_cap_value,
            chronolog_keeper_journal_group_commit_flush_byte_count,
            chronolog_keeper_journal_group_commit_large_payload_byte_count,
            chronolog_keeper_journal_group_commit_large_payload_flush_event_count,
            chronolog_keeper_journal_group_commit_wait_us_value,
            chronolog_keeper_journal_group_commit_flush_wait_us_value,
            chronolog_keeper_journal_owner_drain_yields_value,
            chronolog_keeper_journal_notify_owner_only_on_empty_value,
            chronolog_keeper_append_stats_interval_event_count,
            chronolog_keeper_wal_drain_batch_event_count,
            chronolog_keeper_wal_drain_batch_wait_us_value,
            chronolog_keeper_journal_async_drain_thread_count,
            chronolog_keeper_journal_async_callback_dispatch_value,
            chronolog_keeper_journal_async_batch_completion_dispatch_value,
            chronolog_keeper_journal_callback_dispatch_thread_count,
            chronolog_keeper_journal_callback_batch_drain_value,
            chronolog_keeper_journal_callback_batch_drain_max_value,
            chronolog_keeper_journal_callback_batch_drain_min_payload_bytes_value,
            chronolog_keeper_journal_durable_complete_before_publish_value,
            chronolog_keeper_recording_margo_xstream_count,
            chronolog_keeper_recording_margo_progress_thread,
            chronolog_keeper_recording_margo_handler_count,
            chronolog_grapher_recording_margo_xstream_count,
            chronolog_grapher_recording_margo_handler_count,
            chronolog_grapher_direct_deserialize,
            chronolog_keeper_drain_margo_progress_thread,
            chronolog_keeper_drain_margo_rpc_thread_count,
            chronolog_keeper_extraction_thread_count,
            chronolog_keeper_direct_serialize,
            chronolog_keeper_fast_wire,
            chronolog_keeper_stop_story_flush_drain,
            chronolog_keeper_stop_story_flush_drain_timeout_ms,
            chronolog_visor_parallel_keeper_stop,
        ) in itertools.product(
            backend_pairs,
            mofka_storage_sizes_for_system,
            kafka_acks_modes,
            completion_modes,
            chronolog_poll_intervals_us,
            chronolog_keeper_data_collection_stream_modes,
            chronolog_keeper_data_collection_threads_per_stream_modes,
            chronolog_grapher_data_collection_stream_modes,
            chronolog_grapher_data_collection_threads_per_stream_modes,
            chronolog_grapher_inactive_story_delay_modes,
            chronolog_barrier_modes,
            chronolog_shared_story_modes,
            chronolog_producer_outstanding_modes,
            chronolog_producer_batch_size_modes,
            chronolog_producer_wait_policy_modes,
            chronolog_client_batch_keeper_selection_modes,
            chronolog_client_keeper_time_bucket_ns_modes,
            chronolog_client_parallel_tail_rpc_modes,
            chronolog_client_keeper_cursor_drain_modes,
            chronolog_client_keeper_cursor_drain_max_batch_modes,
            chronolog_client_keeper_cursor_packed_batch_modes,
            chronolog_client_keeper_cursor_packed_bulk_modes,
            chronolog_client_keeper_cursor_packed_bulk_stream_modes,
            chronolog_client_keeper_cursor_packed_bulk_stream_max_batch_modes,
            chronolog_client_keeper_cursor_packed_bulk_buffer_byte_modes,
            chronolog_mixed_tail_read_mode_modes,
            chronolog_mixed_tail_reader_start_mode_modes,
            chronolog_client_execution_mode_values,
            chronolog_grapher_retire_on_stop_modes,
            chronolog_grapher_stop_retire_grace_us_modes,
            chronolog_grapher_stop_story_archive_drain_modes,
            chronolog_grapher_stop_story_archive_drain_timeout_ms_modes,
            chronolog_grapher_extraction_thread_modes,
            chronolog_hdf5_archive_atomic_rename_modes,
            chronolog_hdf5_archive_chunk_event_modes,
            chronolog_hdf5_archive_layout_modes,
            chronolog_raw_blob_preallocate_modes,
            chronolog_raw_blob_async_close_values,
            chronolog_raw_blob_async_publish_values,
            chronolog_raw_blob_async_publish_threads_values,
            chronolog_archive_event_count_poll_interval_modes,
            chronolog_archive_readback_modes,
            chronolog_archive_range_event_count_modes,
            chronolog_keeper_journal_shards,
            chronolog_keeper_journal_shard_policies,
            chronolog_keeper_journal_placement_modes,
            chronolog_keeper_journal_fdatasync_batch_events,
            chronolog_keeper_journal_batch_writev_modes,
            chronolog_keeper_journal_move_batch_payloads_modes,
            chronolog_keeper_tail_batch_max_events,
            chronolog_keeper_tail_batch_max_bytes,
            chronolog_keeper_journal_group_commit_flush_events,
            chronolog_keeper_journal_group_commit_strict_flush_event_cap,
            chronolog_keeper_journal_group_commit_flush_bytes,
            chronolog_keeper_journal_group_commit_large_payload_bytes,
            chronolog_keeper_journal_group_commit_large_payload_flush_events,
            chronolog_keeper_journal_group_commit_wait_us,
            chronolog_keeper_journal_group_commit_flush_wait_us,
            chronolog_keeper_journal_owner_drain_yields,
            chronolog_keeper_journal_notify_owner_only_on_empty,
            chronolog_keeper_append_stats_interval_events,
            chronolog_keeper_wal_drain_batch_events,
            chronolog_keeper_wal_drain_batch_wait_us,
            chronolog_keeper_journal_async_drain_thread_modes,
            chronolog_keeper_journal_async_callback_dispatch_modes,
            chronolog_keeper_journal_async_batch_completion_dispatch_modes,
            chronolog_keeper_journal_callback_dispatch_thread_modes,
            chronolog_keeper_journal_callback_batch_drain_modes,
            chronolog_keeper_journal_callback_batch_drain_max_modes,
            chronolog_keeper_journal_callback_batch_drain_min_payload_bytes_modes,
            chronolog_keeper_journal_durable_complete_before_publish_modes,
            chronolog_keeper_recording_margo_xstream_modes,
            chronolog_keeper_recording_margo_progress_thread_modes,
            chronolog_keeper_recording_margo_handler_modes,
            chronolog_grapher_recording_margo_xstream_modes,
            chronolog_grapher_recording_margo_handler_modes,
            chronolog_grapher_direct_deserialize_modes,
            chronolog_keeper_drain_margo_progress_thread_modes,
            chronolog_keeper_drain_margo_rpc_thread_modes,
            chronolog_keeper_extraction_thread_modes,
            chronolog_keeper_direct_serialize_modes,
            chronolog_keeper_fast_wire_modes,
            chronolog_keeper_stop_story_flush_drain_modes,
            chronolog_keeper_stop_story_flush_drain_timeout_ms_modes,
            chronolog_visor_parallel_keeper_stop_modes,
        ):
            if system == "mofka":
                (
                    mofka_partition_type,
                    mofka_storage_target_type,
                    mofka_producer_wait_mode,
                    mofka_producer_flush_mode,
                    mofka_precreate_storage_provider,
                ) = backend_pair
                mofka_group_ping_timeout_ms = args.mofka_group_ping_timeout_ms
                mofka_group_ping_interval_min_ms = args.mofka_group_ping_interval_min_ms
                mofka_group_ping_interval_max_ms = args.mofka_group_ping_interval_max_ms
                mofka_group_ping_max_timeouts = args.mofka_group_ping_max_timeouts
            else:
                mofka_partition_type = ""
                mofka_storage_target_type = ""
                mofka_storage_target_size = ""
                mofka_producer_wait_mode = ""
                mofka_producer_flush_mode = ""
                mofka_precreate_storage_provider = ""
                mofka_group_ping_timeout_ms = ""
                mofka_group_ping_interval_min_ms = ""
                mofka_group_ping_interval_max_ms = ""
                mofka_group_ping_max_timeouts = ""
            if system != "kafka":
                kafka_acks = ""
            if system == "chronolog" and not chronolog_completion_mode_is_valid_for_workflow(
                workflow, str(chronolog_completion_mode)
            ):
                continue
            if (
                system == "chronolog"
                and workflow == "mixed_append_tail"
                and str(chronolog_mixed_tail_read_mode).startswith("keeper_cursor")
                and not str(chronolog_completion_mode).startswith("keeper_journal_")
            ):
                continue
            runs.append(
                {
                    "system": system,
                    "workflow": workflow,
                    "trial": trial,
                    "node_count": node_count,
                    "nodes": node_count,
                    "message_size_bytes": message_size,
                    "operation_count": op_count,
                    "operation_count_per_client": op_count,
                    "message_count_per_client": op_count,
                    "messages_per_client": op_count,
                    "total_operation_count": op_count * client_count,
                    "total_message_count": op_count * client_count,
                    "total_messages": op_count * client_count,
                    "client_count": client_count,
                    "parallel_clients": client_count,
                    "parallel_client_count": client_count,
                    "total_payload_bytes": message_size * op_count * client_count,
                    "mofka_partition_type": mofka_partition_type,
                    "mofka_storage_target_type": mofka_storage_target_type,
                    "mofka_storage_target_size": mofka_storage_target_size,
                    "mofka_producer_wait_mode": mofka_producer_wait_mode,
                    "mofka_producer_flush_mode": mofka_producer_flush_mode,
                    "mofka_precreate_storage_provider": mofka_precreate_storage_provider,
                    "mofka_group_ping_timeout_ms": mofka_group_ping_timeout_ms,
                    "mofka_group_ping_interval_min_ms": mofka_group_ping_interval_min_ms,
                    "mofka_group_ping_interval_max_ms": mofka_group_ping_interval_max_ms,
                    "mofka_group_ping_max_timeouts": mofka_group_ping_max_timeouts,
                    "kafka_acks": kafka_acks,
                    "chronolog_completion_mode": chronolog_completion_mode,
                    "chronolog_data_collection_poll_interval_us": chronolog_poll_interval_us,
                    "chronolog_archive_event_count_poll_interval_seconds": chronolog_archive_event_count_poll_interval_seconds,
                    "chronolog_archive_readback_mode": chronolog_archive_readback_mode,
                    "chronolog_keeper_data_collection_streams": chronolog_keeper_data_collection_stream_count,
                    "chronolog_keeper_data_collection_threads_per_stream": chronolog_keeper_data_collection_threads_per_stream_count,
                    "chronolog_grapher_data_collection_streams": chronolog_grapher_data_collection_stream_count,
                    "chronolog_grapher_data_collection_threads_per_stream": chronolog_grapher_data_collection_threads_per_stream_count,
                    "chronolog_grapher_inactive_story_delay_seconds": chronolog_grapher_inactive_story_delay_seconds,
                    "chronolog_chrono_bench_barrier": chronolog_barrier_mode,
                    "chronolog_chrono_bench_shared_story": chronolog_shared_story_mode,
                    "chronolog_producer_outstanding": chronolog_producer_outstanding,
                    "chronolog_producer_batch_size": chronolog_producer_batch_size,
                    "chronolog_producer_wait_policy": chronolog_producer_wait_policy,
                    "chronolog_client_batch_keeper_selection": chronolog_client_batch_keeper_selection,
                    "chronolog_client_keeper_time_bucket_ns": chronolog_client_keeper_time_bucket_ns,
                    "chronolog_client_parallel_tail_rpc": chronolog_client_parallel_tail_rpc,
                    "chronolog_client_keeper_cursor_drain": chronolog_client_keeper_cursor_drain,
                    "chronolog_client_keeper_cursor_drain_max_batches": chronolog_client_keeper_cursor_drain_max_batches,
                    "chronolog_client_keeper_cursor_packed_batch": chronolog_client_keeper_cursor_packed_batch,
                    "chronolog_client_keeper_cursor_packed_bulk": chronolog_client_keeper_cursor_packed_bulk,
                    "chronolog_client_keeper_cursor_packed_bulk_stream": chronolog_client_keeper_cursor_packed_bulk_stream,
                    "chronolog_client_keeper_cursor_packed_bulk_stream_max_batches": chronolog_client_keeper_cursor_packed_bulk_stream_max_batches,
                    "chronolog_client_keeper_cursor_packed_bulk_buffer_bytes": chronolog_client_keeper_cursor_packed_bulk_buffer_bytes,
                    "chronolog_mixed_tail_read_mode": chronolog_mixed_tail_read_mode,
                    "chronolog_mixed_tail_reader_start_mode": chronolog_mixed_tail_reader_start_mode,
                    "chronolog_client_execution_mode": chronolog_client_execution_mode,
                    "chronolog_grapher_retire_on_stop": chronolog_grapher_retire_on_stop,
                    "chronolog_grapher_stop_retire_grace_us": chronolog_grapher_stop_retire_grace_us,
                    "chronolog_grapher_stop_story_archive_drain": chronolog_grapher_stop_story_archive_drain,
                    "chronolog_grapher_stop_story_archive_drain_timeout_ms": (
                        chronolog_grapher_stop_story_archive_drain_timeout_ms
                    ),
                    "chronolog_grapher_extraction_threads": chronolog_grapher_extraction_threads,
                    "chronolog_hdf5_archive_atomic_rename": chronolog_hdf5_archive_atomic_rename,
                    "chronolog_hdf5_archive_chunk_events": chronolog_hdf5_archive_chunk_events,
                    "chronolog_hdf5_archive_layout": chronolog_hdf5_archive_layout,
                    "chronolog_raw_blob_preallocate": chronolog_raw_blob_preallocate,
                    "chronolog_raw_blob_async_close": chronolog_raw_blob_async_close,
                    "chronolog_raw_blob_async_publish": chronolog_raw_blob_async_publish,
                    "chronolog_raw_blob_async_publish_threads": chronolog_raw_blob_async_publish_threads,
                    "chronolog_archive_range_event_count": chronolog_archive_range_event_count,
                    "chronolog_producer_wait_mode": chronolog_producer_wait_label(
                        chronolog_producer_wait_policy,
                        chronolog_producer_outstanding or 1,
                        chronolog_producer_batch_size or 1,
                    ),
                    "chronolog_keeper_journal_shards": chronolog_keeper_journal_shard_count,
                    "chronolog_keeper_journal_shard_policy": chronolog_keeper_journal_shard_policy,
                    "chronolog_keeper_journal_placement": chronolog_keeper_journal_placement,
                    "chronolog_keeper_journal_local_base": (
                        args.chronolog_keeper_journal_local_base if chronolog_keeper_journal_placement else ""
                    ),
                    "chronolog_keeper_journal_fdatasync_batch_events": chronolog_keeper_journal_fdatasync_batch_event_count,
                    "chronolog_keeper_journal_batch_writev": chronolog_keeper_journal_batch_writev,
                    "chronolog_keeper_journal_move_batch_payloads": chronolog_keeper_journal_move_batch_payloads,
                    "chronolog_keeper_tail_batch_max_events": chronolog_keeper_tail_batch_max_event_count,
                    "chronolog_keeper_tail_batch_max_bytes": chronolog_keeper_tail_batch_max_byte_count,
                    "keeper_tail_batch_max_events": chronolog_keeper_tail_batch_max_event_count,
                    "keeper_tail_batch_max_bytes": chronolog_keeper_tail_batch_max_byte_count,
                    "chronolog_keeper_journal_group_commit_flush_events": chronolog_keeper_journal_group_commit_flush_event_count,
                    "chronolog_keeper_journal_group_commit_strict_flush_event_cap": (
                        chronolog_keeper_journal_group_commit_strict_flush_event_cap_value
                    ),
                    "chronolog_keeper_journal_group_commit_flush_bytes": chronolog_keeper_journal_group_commit_flush_byte_count,
                    "chronolog_keeper_journal_group_commit_large_payload_bytes": (
                        chronolog_keeper_journal_group_commit_large_payload_byte_count
                    ),
                    "chronolog_keeper_journal_group_commit_large_payload_flush_events": (
                        chronolog_keeper_journal_group_commit_large_payload_flush_event_count
                    ),
                    "chronolog_keeper_journal_group_commit_wait_us": chronolog_keeper_journal_group_commit_wait_us_value,
                    "chronolog_keeper_journal_group_commit_flush_wait_us": chronolog_keeper_journal_group_commit_flush_wait_us_value,
                    "chronolog_keeper_journal_owner_drain_yields": (
                        chronolog_keeper_journal_owner_drain_yields_value
                    ),
                    "chronolog_keeper_journal_notify_owner_only_on_empty": (
                        chronolog_keeper_journal_notify_owner_only_on_empty_value
                    ),
                    "chronolog_keeper_append_stats_interval_events": (
                        chronolog_keeper_append_stats_interval_event_count
                    ),
                    "chronolog_keeper_wal_drain_batch_events": chronolog_keeper_wal_drain_batch_event_count,
                    "chronolog_keeper_wal_drain_batch_wait_us": chronolog_keeper_wal_drain_batch_wait_us_value,
                    "chronolog_keeper_journal_async_drain_threads": (
                        chronolog_keeper_journal_async_drain_thread_count
                    ),
                    "chronolog_keeper_journal_async_callback_dispatch": (
                        chronolog_keeper_journal_async_callback_dispatch_value
                    ),
                    "chronolog_keeper_journal_async_batch_completion_dispatch": (
                        chronolog_keeper_journal_async_batch_completion_dispatch_value
                    ),
                    "chronolog_keeper_journal_callback_dispatch_threads": (
                        chronolog_keeper_journal_callback_dispatch_thread_count
                    ),
                    "chronolog_keeper_journal_callback_batch_drain": chronolog_keeper_journal_callback_batch_drain_value,
                    "chronolog_keeper_journal_callback_batch_drain_max": (
                        chronolog_keeper_journal_callback_batch_drain_max_value
                    ),
                    "chronolog_keeper_journal_callback_batch_drain_min_payload_bytes": (
                        chronolog_keeper_journal_callback_batch_drain_min_payload_bytes_value
                    ),
                    "chronolog_keeper_journal_durable_complete_before_publish": (
                        chronolog_keeper_journal_durable_complete_before_publish_value
                    ),
                    "chronolog_keeper_recording_margo_xstreams": chronolog_keeper_recording_margo_xstream_count,
                    "chronolog_keeper_recording_margo_progress_thread": (
                        chronolog_keeper_recording_margo_progress_thread
                    ),
                    "chronolog_keeper_recording_margo_handlers": chronolog_keeper_recording_margo_handler_count,
                    "chronolog_grapher_recording_margo_xstreams": chronolog_grapher_recording_margo_xstream_count,
                    "chronolog_grapher_recording_margo_handlers": chronolog_grapher_recording_margo_handler_count,
                    "chronolog_grapher_direct_deserialize": chronolog_grapher_direct_deserialize,
                    "chronolog_keeper_drain_margo_progress_thread": chronolog_keeper_drain_margo_progress_thread,
                    "chronolog_keeper_drain_margo_rpc_threads": chronolog_keeper_drain_margo_rpc_thread_count,
                    "chronolog_keeper_extraction_threads": chronolog_keeper_extraction_thread_count,
                    "chronolog_keeper_direct_serialize": chronolog_keeper_direct_serialize,
                    "chronolog_keeper_fast_wire": chronolog_keeper_fast_wire,
                    "chronolog_keeper_stop_story_flush_drain": chronolog_keeper_stop_story_flush_drain,
                    "chronolog_keeper_stop_story_flush_drain_timeout_ms": (
                        chronolog_keeper_stop_story_flush_drain_timeout_ms
                    ),
                    "chronolog_visor_parallel_keeper_stop": chronolog_visor_parallel_keeper_stop,
                    **semantic_boundary(
                        system,
                        workflow,
                        mofka_partition_type,
                        mofka_storage_target_type,
                        mofka_producer_wait_mode,
                        mofka_producer_flush_mode,
                        chronolog_completion_mode,
                        kafka_acks or "1",
                    ),
            }
        )

    for run in runs:
        if run.get("system") != "chronolog" or run.get("workflow") != "mixed_append_tail":
            continue
        mixed_tail_mode = str(run.get("chronolog_mixed_tail_read_mode") or "full")
        if mixed_tail_mode == "keeper_cursor":
            max_bytes = str(run.get("keeper_tail_batch_max_bytes") or run.get("chronolog_keeper_tail_batch_max_bytes") or "")
            max_events = str(
                run.get("keeper_tail_batch_max_events") or run.get("chronolog_keeper_tail_batch_max_events") or ""
            )
            if max_bytes not in {"", "0"} or max_events not in {"", "0"}:
                suffix_parts: list[str] = []
                if max_bytes not in {"", "0"}:
                    try:
                        max_bytes_int = int(max_bytes)
                    except ValueError:
                        suffix_parts.append(f"{max_bytes}B")
                    else:
                        if max_bytes_int % (1024 * 1024) == 0:
                            suffix_parts.append(f"{max_bytes_int // (1024 * 1024)}MiB")
                        else:
                            suffix_parts.append(f"{max_bytes_int}B")
                if max_events not in {"", "0"}:
                    suffix_parts.append(f"{max_events}events")
                mixed_tail_mode = f"{mixed_tail_mode}_bounded_tail_batch_{'_'.join(suffix_parts) or 'bounded'}"
        mixed_tail_start = str(run.get("chronolog_mixed_tail_reader_start_mode") or "ready")
        run["semantic_boundary"] = (
            f"mixed_append_concurrent_keeper_journal_tail_{mixed_tail_mode}"
            if mixed_tail_start == "ready"
            else f"append_then_keeper_journal_tail_catchup_{mixed_tail_mode}"
        )
        if str(run.get("chronolog_keeper_journal_durable_complete_before_publish") or "0") == "1":
            run["append_ack_boundary"] = (
                "deferred_rpc_response_after_keeper_journal_group_commit_fdatasync_before_tail_publish"
            )
        run["durability_boundary"] = "keeper_local_journal_group_commit_fdatasync"
        run["read_path"] = (
            "concurrent_keeper_local_journal_tail"
            if mixed_tail_start == "ready"
            else "keeper_local_journal_tail_catchup_after_append"
        )
        run["storage_backend"] = "chronolog_keeper_local_journal"
        if "bounded_tail_batch" in mixed_tail_mode:
            run["semantic_notes"] = (
                "Dry-run command requests bounded Keeper cursor journal-tail RPC batches; keep this as a "
                "streaming/cursor catch-up semantic separate from full ReplayStory."
            )

    if args.shuffle_runs:
        random.Random(args.shuffle_seed).shuffle(runs)
    for run_index, run in enumerate(runs, start=1):
        run["matrix_run_index"] = run_index
        run["matrix_shuffle_enabled"] = bool(args.shuffle_runs)
        run["matrix_shuffle_seed"] = args.shuffle_seed if args.shuffle_runs else None

    (matrix_dir / "matrix-expanded.json").write_text(json.dumps(runs, indent=2) + "\n", encoding="utf-8")
    rows: list[dict[str, Any]] = []
    command_lines: list[str] = []
    for index, run in enumerate(runs, start=1):
        backend_suffix = ""
        if run["system"] == "mofka":
            backend_suffix = (
                f"-{run['mofka_partition_type']}-{run['mofka_storage_target_type']}"
                f"-{run['mofka_producer_wait_mode']}-{run['mofka_producer_flush_mode']}"
            )
        child_dir = root / (
            f"{index:03d}-{run['system']}-{run['workflow']}{backend_suffix}"
            f"-n{run['node_count']}-c{run['client_count']}-s{run['message_size_bytes']}-o{run['operation_count']}-t{run['trial']}"
        )
        cmd = command_for(run, child_dir, args)
        command_lines.append(" ".join(subprocess.list2cmdline([part]) for part in cmd))
        if args.dry_run:
            row = dict(run)
            row.update({"success": None, "error": "dry-run", "result_dir": str(child_dir), "metrics_path": ""})
            annotate_benchmark_validity(row)
            rows.append(row)
            continue
        child_dir.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["PHASE0_RESULT_DIR"] = str(child_dir)
        if run["system"] == "chronolog":
            env["CHRONOLOG_STARTUP_SLEEP_SECONDS"] = str(args.chronolog_startup_sleep)
            env["CHRONOLOG_DEPLOY_STOP_TIMEOUT_SECONDS"] = str(args.chronolog_deploy_stop_timeout_seconds)
            env["CHRONOLOG_KEEPER_JOURNAL_ASYNC_CALLBACK_DISPATCH"] = str(
                run["chronolog_keeper_journal_async_callback_dispatch"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_ASYNC_BATCH_COMPLETION_DISPATCH"] = str(
                run["chronolog_keeper_journal_async_batch_completion_dispatch"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_CALLBACK_DISPATCH_THREADS"] = str(
                run["chronolog_keeper_journal_callback_dispatch_threads"]
            )
            env["CHRONOLOG_KEEPER_RECORDING_MARGO_XSTREAMS"] = str(
                run["chronolog_keeper_recording_margo_xstreams"]
            )
            env["CHRONOLOG_KEEPER_RECORDING_MARGO_PROGRESS_THREAD"] = str(
                run["chronolog_keeper_recording_margo_progress_thread"]
            )
            env["CHRONOLOG_KEEPER_RECORDING_MARGO_HANDLERS"] = str(
                run["chronolog_keeper_recording_margo_handlers"]
            )
            env["CHRONOLOG_KEEPER_DRAIN_MARGO_PROGRESS_THREAD"] = str(
                run["chronolog_keeper_drain_margo_progress_thread"]
            )
            env["CHRONOLOG_KEEPER_DRAIN_MARGO_RPC_THREADS"] = str(run["chronolog_keeper_drain_margo_rpc_threads"])
            env["CHRONOLOG_KEEPER_EXTRACTION_THREADS"] = str(run["chronolog_keeper_extraction_threads"])
            env["CHRONOLOG_KEEPER_DIRECT_SERIALIZE"] = str(run["chronolog_keeper_direct_serialize"])
            env["CHRONOLOG_KEEPER_FAST_WIRE"] = str(run["chronolog_keeper_fast_wire"])
            env["CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN"] = str(
                run["chronolog_keeper_stop_story_flush_drain"]
            )
            env["CHRONOLOG_KEEPER_STOP_STORY_FLUSH_DRAIN_TIMEOUT_MS"] = str(
                run["chronolog_keeper_stop_story_flush_drain_timeout_ms"]
            )
            env["CHRONOLOG_GRAPHER_DIRECT_DESERIALIZE"] = str(run["chronolog_grapher_direct_deserialize"])
            env["CHRONOLOG_GRAPHER_STOP_RETIRE_GRACE_US"] = str(
                run["chronolog_grapher_stop_retire_grace_us"]
            )
            env["CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN"] = str(
                run["chronolog_grapher_stop_story_archive_drain"]
            )
            env["CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN_TIMEOUT_MS"] = str(
                run["chronolog_grapher_stop_story_archive_drain_timeout_ms"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN"] = str(
                run["chronolog_keeper_journal_callback_batch_drain"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MAX"] = str(
                run["chronolog_keeper_journal_callback_batch_drain_max"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_CALLBACK_BATCH_DRAIN_MIN_PAYLOAD_BYTES"] = str(
                run["chronolog_keeper_journal_callback_batch_drain_min_payload_bytes"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH"] = str(
                run["chronolog_keeper_journal_durable_complete_before_publish"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_BATCH_WRITEV"] = str(run["chronolog_keeper_journal_batch_writev"])
            env["CHRONOLOG_KEEPER_JOURNAL_MOVE_BATCH_PAYLOADS"] = str(
                run["chronolog_keeper_journal_move_batch_payloads"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_FLUSH_BYTES"] = str(
                run["chronolog_keeper_journal_group_commit_flush_bytes"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_BYTES"] = str(
                run["chronolog_keeper_journal_group_commit_large_payload_bytes"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_GROUP_COMMIT_LARGE_PAYLOAD_FLUSH_EVENTS"] = str(
                run["chronolog_keeper_journal_group_commit_large_payload_flush_events"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_OWNER_DRAIN_YIELDS"] = str(
                run["chronolog_keeper_journal_owner_drain_yields"]
            )
            env["CHRONOLOG_KEEPER_JOURNAL_NOTIFY_OWNER_ONLY_ON_EMPTY"] = str(
                run["chronolog_keeper_journal_notify_owner_only_on_empty"]
            )
            env["CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION"] = str(
                run["chronolog_client_batch_keeper_selection"]
            )
            env["CHRONOLOG_CLIENT_KEEPER_TIME_BUCKET_NS"] = str(run["chronolog_client_keeper_time_bucket_ns"])
            env["CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC"] = str(run["chronolog_client_parallel_tail_rpc"])
            env["CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN"] = str(run["chronolog_client_keeper_cursor_drain"])
            env["CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES"] = str(
                run["chronolog_client_keeper_cursor_drain_max_batches"]
            )
            env["CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH"] = str(
                run["chronolog_client_keeper_cursor_packed_batch"]
            )
            env["CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK"] = str(
                run["chronolog_client_keeper_cursor_packed_bulk"]
            )
            env["CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM"] = str(
                run["chronolog_client_keeper_cursor_packed_bulk_stream"]
            )
            env["CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES"] = str(
                run["chronolog_client_keeper_cursor_packed_bulk_stream_max_batches"]
            )
            env["CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES"] = str(
                run["chronolog_client_keeper_cursor_packed_bulk_buffer_bytes"]
            )
            env["CHRONOLOG_CLIENT_EXECUTION_MODE"] = str(run["chronolog_client_execution_mode"])
        proc = subprocess.run(cmd, cwd=REPO_ROOT, env=env, text=True)
        row = flatten_metrics(metrics_path(run["system"], child_dir), run)
        if proc.returncode != 0:
            row["success"] = False
            row["error"] = f"command exited {proc.returncode}"
        apply_chronolog_service_health(row, child_dir)
        apply_mofka_service_health(row, child_dir)
        rows.append(row)
        write_summary(rows, matrix_dir / "summary.csv")
        (matrix_dir / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")

    (matrix_dir / "commands.sh").write_text("\n".join(command_lines) + "\n", encoding="utf-8")
    write_summary(rows, matrix_dir / "summary.csv")
    (matrix_dir / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    write_skipped_unsupported_client_count(
        skipped_unsupported_client_count, matrix_dir / "unsupported-client-count.csv"
    )
    (matrix_dir / "unsupported-client-count.json").write_text(
        json.dumps(skipped_unsupported_client_count, indent=2) + "\n",
        encoding="utf-8",
    )
    (root / "summary.md").write_text(
        "# Phase 0 Benchmark Matrix\n\n"
        f"- runs: {len(runs)}\n"
        f"- skipped_unsupported_client_count: {len(skipped_unsupported_client_count)}\n"
        f"- dry_run: {args.dry_run}\n"
        f"- summary: benchmark-matrix/summary.csv\n",
        encoding="utf-8",
    )
    print(matrix_dir)
    return 0 if all(row.get("success") in {True, None} for row in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
