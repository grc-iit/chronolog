#!/usr/bin/env python3
"""Generate a reproducible semantic benchmark-matrix plan.

The plan is intentionally separate from the matrix runner.  It records the
matched semantic cells we intend to run and emits one concrete runner command
per size so message-size and operation-count stay paired.
"""

from __future__ import annotations

import argparse
import json
import shlex
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
MATRIX = REPO_ROOT / ".agent" / "scripts" / "phase0_benchmark_matrix.py"


@dataclass(frozen=True)
class Cell:
    name: str
    system: str
    workflow: str
    semantic_class: str
    notes: str
    extra_args: tuple[str, ...] = ()
    status: str = "planned"
    blocked_reason: str = ""
    evidence: tuple[str, ...] = ()


def parse_size_ops(value: str) -> list[tuple[int, int]]:
    pairs: list[tuple[int, int]] = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        size_text, ops_text = item.split(":", 1)
        pairs.append((int(size_text), int(ops_text)))
    if not pairs:
        raise ValueError("at least one size:operations pair is required")
    return pairs


def cells(include_blocked_storage: bool) -> list[Cell]:
    base: list[Cell] = [
        Cell(
            name="chronolog_live_memory_append",
            system="chronolog",
            workflow="append_throughput",
            semantic_class="memory_live_append",
            notes="ChronoLog live return from record_event; not a durability/storage claim.",
            extra_args=(
                "--chronolog-completion-modes",
                "live_return",
                "--chronolog-hdf5-archive-layouts",
                "raw_blob",
                "--chronolog-hdf5-archive-atomic-rename-values",
                "1",
                "--chronolog-grapher-extraction-threads",
                "2",
                "--chronolog-grapher-inactive-story-delay-seconds",
                "3",
                "--chronolog-chrono-bench-barrier-modes",
                "true",
                "--chronolog-chrono-bench-shared-story-modes",
                "true",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="chronolog_journal_durable_restart_tail",
            system="chronolog",
            workflow="keeper_restart_recovery",
            semantic_class="keeper_journal_durable_tail",
            notes="Keeper-local journal group-commit fdatasync with restart/readback gate; not archive storage.",
            extra_args=(
                "--chronolog-completion-modes",
                "keeper_journal_group_commit_deferred_tail_only",
                "--chronolog-keeper-journal-batch-writev-values",
                "1",
                "--chronolog-keeper-journal-group-commit-flush-events",
                "64",
                "--chronolog-keeper-journal-group-commit-wait-us",
                "0",
                "--chronolog-keeper-journal-group-commit-flush-wait-us",
                "0",
                "--chronolog-chrono-bench-barrier-modes",
                "true",
                "--chronolog-chrono-bench-shared-story-modes",
                "true",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="chronolog_journal_durable_restart_tail_outstanding64",
            system="chronolog",
            workflow="keeper_restart_recovery",
            semantic_class="keeper_journal_durable_tail_bounded_outstanding",
            notes=(
                "Keeper-local journal group-commit fdatasync with restart/readback gate and 64 bounded "
                "in-flight async appends per client; this preserves journal durability evidence but changes "
                "the producer wait behavior so group commit can actually batch."
            ),
            extra_args=(
                "--chronolog-completion-modes",
                "keeper_journal_group_commit_deferred_tail_only",
                "--chronolog-keeper-journal-batch-writev-values",
                "1",
                "--chronolog-keeper-journal-group-commit-flush-events",
                "64",
                "--chronolog-keeper-journal-group-commit-wait-us",
                "0",
                "--chronolog-keeper-journal-group-commit-flush-wait-us",
                "0",
                "--chronolog-producer-outstanding-values",
                "64",
                "--chronolog-client-execution-modes",
                "threads",
                "--chronolog-chrono-bench-barrier-modes",
                "false",
                "--chronolog-chrono-bench-shared-story-modes",
                "false",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="chronolog_journal_durable_restart_tail_afterloop_batch64",
            system="chronolog",
            workflow="keeper_restart_recovery",
            semantic_class="keeper_journal_durable_tail_afterloop_batch",
            notes=(
                "Keeper-local journal group-commit fdatasync with restart/readback gate, async client "
                "batches of 64 records, and after-loop wait. This is a separate durable wait policy, "
                "matching the producer wait knob used for Kafka/Mofka-style deferred waits."
            ),
            extra_args=(
                "--chronolog-completion-modes",
                "keeper_journal_group_commit_deferred_tail_only",
                "--chronolog-keeper-journal-batch-writev-values",
                "1",
                "--chronolog-keeper-journal-group-commit-flush-events",
                "64",
                "--chronolog-keeper-journal-group-commit-wait-us",
                "0",
                "--chronolog-keeper-journal-group-commit-flush-wait-us",
                "0",
                "--chronolog-producer-outstanding-values",
                "64",
                "--chronolog-producer-batch-sizes",
                "64",
                "--chronolog-producer-wait-modes",
                "after_loop",
                "--chronolog-client-execution-modes",
                "threads",
                "--chronolog-chrono-bench-barrier-modes",
                "false",
                "--chronolog-chrono-bench-shared-story-modes",
                "false",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="chronolog_journal_durable_restart_tail_afterloop_batch16",
            system="chronolog",
            workflow="keeper_restart_recovery",
            semantic_class="keeper_journal_durable_tail_afterloop_batch",
            notes=(
                "Keeper-local journal group-commit fdatasync with restart/readback gate, async client "
                "batches of 16 records, and after-loop wait. Current evidence makes this the better "
                "64KiB candidate, while batch64 remains better for the current 1KiB guardrail."
            ),
            extra_args=(
                "--chronolog-completion-modes",
                "keeper_journal_group_commit_deferred_tail_only",
                "--chronolog-keeper-journal-batch-writev-values",
                "1",
                "--chronolog-keeper-journal-group-commit-flush-events",
                "64",
                "--chronolog-keeper-journal-group-commit-wait-us",
                "0",
                "--chronolog-keeper-journal-group-commit-flush-wait-us",
                "0",
                "--chronolog-producer-outstanding-values",
                "64",
                "--chronolog-producer-batch-sizes",
                "16",
                "--chronolog-producer-wait-modes",
                "after_loop",
                "--chronolog-client-execution-modes",
                "threads",
                "--chronolog-chrono-bench-barrier-modes",
                "false",
                "--chronolog-chrono-bench-shared-story-modes",
                "false",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="chronolog_journal_durable_restart_tail_bounded16_batch16",
            system="chronolog",
            workflow="keeper_restart_recovery",
            semantic_class="keeper_journal_durable_tail_bounded_backpressure",
            notes=(
                "Keeper-local journal group-commit fdatasync with restart/readback gate, 16-record "
                "client batches, and bounded outstanding window 16. This is a size-sensitive "
                "backpressure candidate driven by owner-queue profiling evidence."
            ),
            extra_args=(
                "--chronolog-completion-modes",
                "keeper_journal_group_commit_deferred_tail_only",
                "--chronolog-keeper-journal-batch-writev-values",
                "1",
                "--chronolog-keeper-journal-group-commit-flush-events",
                "64",
                "--chronolog-keeper-journal-group-commit-wait-us",
                "0",
                "--chronolog-keeper-journal-group-commit-flush-wait-us",
                "0",
                "--chronolog-producer-outstanding-values",
                "16",
                "--chronolog-producer-batch-sizes",
                "16",
                "--chronolog-producer-wait-modes",
                "bounded_outstanding",
                "--chronolog-client-execution-modes",
                "threads",
                "--chronolog-chrono-bench-barrier-modes",
                "false",
                "--chronolog-chrono-bench-shared-story-modes",
                "false",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="chronolog_journal_durable_restart_tail_bounded16_batch16_move_payloads",
            system="chronolog",
            workflow="keeper_restart_recovery",
            semantic_class="keeper_journal_durable_tail_bounded_backpressure_copy_reduced",
            notes=(
                "Keeper-local journal group-commit fdatasync with restart/readback gate, 16-record "
                "client batches, bounded outstanding window 16, and Keeper journal batch payload move "
                "enabled. This is a copy-reduction candidate driven by the bounded16 gperftools payload-copy signal."
            ),
            extra_args=(
                "--chronolog-completion-modes",
                "keeper_journal_group_commit_deferred_tail_only",
                "--chronolog-keeper-journal-batch-writev-values",
                "1",
                "--chronolog-keeper-journal-move-batch-payloads-values",
                "1",
                "--chronolog-keeper-journal-group-commit-flush-events",
                "64",
                "--chronolog-keeper-journal-group-commit-wait-us",
                "0",
                "--chronolog-keeper-journal-group-commit-flush-wait-us",
                "0",
                "--chronolog-producer-outstanding-values",
                "16",
                "--chronolog-producer-batch-sizes",
                "16",
                "--chronolog-producer-wait-modes",
                "bounded_outstanding",
                "--chronolog-client-execution-modes",
                "threads",
                "--chronolog-chrono-bench-barrier-modes",
                "false",
                "--chronolog-chrono-bench-shared-story-modes",
                "false",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="chronolog_archive_storage_range",
            system="chronolog",
            workflow="archive_range_retrieval",
            semantic_class="archive_storage_range",
            notes="Append/release, archive event-count wait, and ChronoPlayer HDF5 readback.",
            extra_args=(
                "--chronolog-completion-modes",
                "archive_readback",
                "--chronolog-hdf5-archive-layouts",
                "raw_blob",
                "--chronolog-hdf5-archive-atomic-rename-values",
                "1",
                "--chronolog-grapher-extraction-threads",
                "2",
                "--chronolog-keeper-stop-story-flush-drain-values",
                "1",
                "--chronolog-keeper-stop-story-flush-drain-timeout-ms",
                "120000",
                "--chronolog-archive-event-count-poll-intervals-seconds",
                "1.0",
                "--chronolog-chrono-bench-barrier-modes",
                "true",
                "--chronolog-chrono-bench-shared-story-modes",
                "true",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="chronolog_mixed_keeper_tail",
            system="chronolog",
            workflow="mixed_append_tail",
            semantic_class="mixed_live_tail",
            notes="Concurrent append and Keeper-tail cursor reads under the runner-supported live-return completion mode; classify with tail counters before comparing.",
            extra_args=(
                "--chronolog-completion-modes",
                "live_return",
                "--chronolog-hdf5-archive-layouts",
                "raw_blob",
                "--chronolog-hdf5-archive-atomic-rename-values",
                "1",
                "--chronolog-grapher-extraction-threads",
                "2",
                "--chronolog-grapher-inactive-story-delay-seconds",
                "3",
                "--chronolog-client-execution-modes",
                "parallel",
                "--chronolog-client-parallel-tail-rpc-values",
                "1",
                "--chronolog-chrono-bench-barrier-modes",
                "true",
                "--chronolog-chrono-bench-shared-story-modes",
                "true",
                "--chronolog-deploy-stop-timeout-seconds",
                "180",
            ),
        ),
        Cell(
            name="kafka_acks0_append",
            system="kafka",
            workflow="append_throughput",
            semantic_class="memory_live_append",
            notes="Kafka producer acks=0 fixed baseline.",
            extra_args=("--kafka-acks-values", "0"),
        ),
        Cell(
            name="kafka_acksall_append_rf1",
            system="kafka",
            workflow="append_throughput",
            semantic_class="leader_ack_or_log_append",
            notes="Kafka acks=all, replication factor 1 fixed baseline.",
            extra_args=("--kafka-acks-values", "all"),
        ),
        Cell(
            name="kafka_acks0_catchup",
            system="kafka",
            workflow="range_retrieval",
            semantic_class="append_then_catchup",
            notes="Kafka append then consumer catch-up with acks=0; not archive subrange semantics.",
            extra_args=("--kafka-acks-values", "0"),
        ),
        Cell(
            name="kafka_acksall_catchup_rf1",
            system="kafka",
            workflow="range_retrieval",
            semantic_class="append_then_catchup",
            notes="Kafka append then consumer catch-up with acks=all/rf1; not archive subrange semantics.",
            extra_args=("--kafka-acks-values", "all"),
        ),
        Cell(
            name="mofka_memory_nowait_noflush_append",
            system="mofka",
            workflow="append_throughput",
            semantic_class="memory_live_append",
            notes="Mofka memory partition, push without explicit wait and no explicit flush.",
            extra_args=(
                "--mofka-partition-types",
                "memory",
                "--mofka-storage-target-types",
                "memory",
                "--mofka-producer-wait-modes",
                "none",
                "--mofka-producer-flush-modes",
                "no_flush",
            ),
        ),
        Cell(
            name="mofka_memory_wait_flush_append",
            system="mofka",
            workflow="append_throughput",
            semantic_class="memory_live_append_wait_flush",
            notes="Mofka memory partition with per-event wait and flush-after-loop.",
            extra_args=(
                "--mofka-partition-types",
                "memory",
                "--mofka-storage-target-types",
                "memory",
                "--mofka-producer-wait-modes",
                "per_event",
                "--mofka-producer-flush-modes",
                "after_loop",
            ),
        ),
        Cell(
            name="mofka_memory_afterloop_pull_catchup",
            system="mofka",
            workflow="range_retrieval",
            semantic_class="append_then_catchup",
            notes="Mofka memory partition append then consumer pull catch-up.",
            extra_args=(
                "--mofka-partition-types",
                "memory",
                "--mofka-storage-target-types",
                "memory",
                "--mofka-producer-wait-modes",
                "after_loop",
                "--mofka-producer-flush-modes",
                "no_flush",
            ),
        ),
    ]
    storage = [
        Cell(
            name="mofka_storage_pmdk_nowait_noflush_append",
            system="mofka",
            workflow="append_throughput",
            semantic_class="storage_backed_append_no_wait",
            notes=(
                "Mofka default partition, PMDK/Warabi storage target, push without explicit wait and no "
                "explicit flush. This is storage-backed placement evidence, but not a synchronous completion claim. "
                "Accepted evidence currently covers 1KiB and 64KiB non-smoke rows; the 64KiB row requires "
                "relaxed Flock group ping settings so large PMDK target startup does not evict live members."
            ),
            extra_args=(
                "--mofka-partition-types",
                "default",
                "--mofka-storage-target-types",
                "pmdk",
                "--mofka-storage-target-sizes",
                "3221225472",
                "--mofka-producer-wait-modes",
                "none",
                "--mofka-producer-flush-modes",
                "no_flush",
                "--mofka-precreate-storage-provider-values",
                "yes",
                "--mofka-group-ping-timeout-ms",
                "10000",
                "--mofka-group-ping-interval-min-ms",
                "5000",
                "--mofka-group-ping-interval-max-ms",
                "5000",
                "--mofka-group-ping-max-timeouts",
                "12",
            ),
            evidence=(
                ".agent/results/20260516-150500-mofka-pmdk-storage-nowait-1k",
                ".agent/results/20260516-154000-mofka-pmdk-64k-group-timeout-probe",
            ),
        ),
        Cell(
            name="mofka_storage_pmdk_afterloop_flush_append",
            system="mofka",
            workflow="append_throughput",
            semantic_class="storage_backed_append_deferred_wait_flush",
            notes=(
                "Mofka default partition, PMDK/Warabi storage target, deferred wait after submission loop "
                "plus flush-after-loop. Accepted evidence currently covers 1KiB and 64KiB non-smoke rows; "
                "the 64KiB row requires relaxed Flock group ping settings so large PMDK target startup does "
                "not evict live members."
            ),
            extra_args=(
                "--mofka-partition-types",
                "default",
                "--mofka-storage-target-types",
                "pmdk",
                "--mofka-storage-target-sizes",
                "3221225472",
                "--mofka-producer-wait-modes",
                "after_loop",
                "--mofka-producer-flush-modes",
                "after_loop",
                "--mofka-precreate-storage-provider-values",
                "yes",
                "--mofka-group-ping-timeout-ms",
                "10000",
                "--mofka-group-ping-interval-min-ms",
                "5000",
                "--mofka-group-ping-interval-max-ms",
                "5000",
                "--mofka-group-ping-max-timeouts",
                "12",
            ),
            evidence=(
                ".agent/results/20260516-151000-mofka-pmdk-storage-afterloop-1k",
                ".agent/results/20260516-155000-mofka-pmdk-64k-afterloop-group-timeout",
            ),
        ),
        Cell(
            name="mofka_storage_pmdk_wait_flush_append",
            system="mofka",
            workflow="append_throughput",
            semantic_class="storage_backed_append",
            notes=(
                "Mofka default partition, PMDK/Warabi storage target, per-event wait, flush-after-loop. "
                "The 1KiB non-smoke row is accepted with relaxed Flock ping settings and 3GiB PMDK targets; "
                "the 64KiB non-smoke row is accepted with relaxed Flock ping settings and 4GiB PMDK targets."
            ),
            extra_args=(
                "--mofka-partition-types",
                "default",
                "--mofka-storage-target-types",
                "pmdk",
                "--mofka-storage-target-sizes",
                "3221225472",
                "--mofka-producer-wait-modes",
                "per_event",
                "--mofka-producer-flush-modes",
                "after_loop",
                "--mofka-precreate-storage-provider-values",
                "yes",
                "--mofka-group-ping-timeout-ms",
                "10000",
                "--mofka-group-ping-interval-min-ms",
                "5000",
                "--mofka-group-ping-interval-max-ms",
                "5000",
                "--mofka-group-ping-max-timeouts",
                "12",
            ),
            status="planned",
            blocked_reason="",
            evidence=(
                ".agent/results/20260518-205732-mofka-pmdk-per-event-flush-1k-retry",
                ".agent/results/20260518-210314-mofka-pmdk-per-event-flush-64k-retry",
                ".agent/results/20260515-194500-mofka-storage-launch-diagnostics/summary.md",
            ),
        ),
        Cell(
            name="mofka_storage_pmdk_afterloop_pull_catchup",
            system="mofka",
            workflow="range_retrieval",
            semantic_class="storage_backed_catchup",
            notes="Mofka default partition, PMDK/Warabi storage target, deferred wait then consumer pull.",
            extra_args=(
                "--mofka-partition-types",
                "default",
                "--mofka-storage-target-types",
                "pmdk",
                "--mofka-producer-wait-modes",
                "after_loop",
                "--mofka-producer-flush-modes",
                "after_loop",
                "--mofka-precreate-storage-provider-values",
                "yes",
            ),
            status="planned" if include_blocked_storage else "blocked",
            blocked_reason=(
                ""
                if include_blocked_storage
                else "Mofka PMDK/default service currently fails health gate with storage-service startup segfault."
            ),
            evidence=(".agent/results/20260515-194500-mofka-storage-launch-diagnostics/summary.md",),
        ),
    ]
    return base + storage


def matrix_command(
    cell: Cell,
    *,
    size: int,
    operations: int,
    nodes: int,
    clients: int,
    trials: int,
    partition: str,
    slurm_time: str,
    result_root: Path,
    nodelist: str,
    dry_run: bool,
) -> list[str]:
    result_dir = result_root / f"{cell.name}-n{nodes}-c{clients}-s{size}-o{operations}"
    cmd = [
        "python3",
        str(MATRIX.relative_to(REPO_ROOT)),
        "--systems",
        cell.system,
        "--workflows",
        cell.workflow,
        "--node-counts",
        str(nodes),
        "--client-counts",
        str(clients),
        "--message-sizes",
        str(size),
        "--operation-counts",
        str(operations),
        "--trials",
        str(trials),
        "--partition",
        partition,
        "--slurm-time",
        slurm_time,
        "--result-dir",
        str(result_dir),
    ]
    if nodelist:
        cmd.extend(["--nodelist", nodelist])
    cmd.extend(cell.extra_args)
    if dry_run:
        cmd.append("--dry-run")
    return cmd


def shell_join(cmd: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in cmd)


def build_plan(args: argparse.Namespace) -> dict[str, Any]:
    size_ops = parse_size_ops(args.size_operations)
    result_root = Path(args.matrix_result_root)
    if not result_root.is_absolute():
        result_root = REPO_ROOT / result_root
    generated_cells = cells(args.include_blocked_storage_mofka)
    planned: list[dict[str, Any]] = []
    commands: list[str] = []
    for cell in generated_cells:
        cell_record: dict[str, Any] = {
            "name": cell.name,
            "system": cell.system,
            "workflow": cell.workflow,
            "semantic_class": cell.semantic_class,
            "status": cell.status,
            "notes": cell.notes,
            "blocked_reason": cell.blocked_reason,
            "evidence": list(cell.evidence),
            "runs": [],
        }
        if cell.status == "planned":
            for size, operations in size_ops:
                total_operations = operations * args.clients
                total_payload_bytes = total_operations * size
                cmd = matrix_command(
                    cell,
                    size=size,
                    operations=operations,
                    nodes=args.nodes,
                    clients=args.clients,
                    trials=args.trials,
                    partition=args.partition,
                    slurm_time=args.slurm_time,
                    result_root=result_root,
                    nodelist=args.nodelist,
                    dry_run=args.dry_run_commands,
                )
                command_text = shell_join(cmd)
                commands.append(command_text)
                cell_record["runs"].append(
                    {
                        "node_count": args.nodes,
                        "nodes": args.nodes,
                        "client_count": args.clients,
                        "parallel_clients": args.clients,
                        "message_size_bytes": size,
                        "operation_count": operations,
                        "operation_count_per_client": operations,
                        "message_count_per_client": operations,
                        "messages_per_client": operations,
                        "total_operation_count": total_operations,
                        "total_message_count": total_operations,
                        "total_messages": total_operations,
                        "total_payload_bytes": total_payload_bytes,
                        "parallel_client_count": args.clients,
                        "trials": args.trials,
                        "command": command_text,
                    }
                )
        planned.append(cell_record)
    return {
        "timestamp": datetime.now().astimezone().strftime("%Y-%m-%d %H:%M %Z"),
        "purpose": "Executable semantic benchmark-matrix plan with paired message-size and operation-count knobs.",
        "rules": {
            "chronolog_is_only_modifiable_target": True,
            "kafka_fixed_baseline_only": True,
            "mofka_fixed_baseline_only": True,
            "memory_and_storage_semantics_are_separate": True,
            "small_100_or_1000_event_runs_are_smoke_only": True,
            "required_metric_count_fields": [
                "node_count",
                "client_count",
                "message_size_bytes",
                "operation_count",
                "operation_count_per_client",
                "message_count_per_client",
                "total_operation_count",
                "total_message_count",
                "total_payload_bytes",
                "parallel_client_count",
            ],
        },
        "default_knobs": {
            "node_count": args.nodes,
            "client_count": args.clients,
            "size_operations": [{"message_size_bytes": size, "operation_count": ops} for size, ops in size_ops],
            "trials": args.trials,
            "partition": args.partition,
            "slurm_time": args.slurm_time,
            "nodelist": args.nodelist,
            "dry_run_commands": args.dry_run_commands,
        },
        "cells": planned,
        "commands": commands,
    }


def write_outputs(plan: dict[str, Any], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "plan.json").write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
    (output_dir / "commands.sh").write_text("\n".join(plan["commands"]) + "\n", encoding="utf-8")
    blocked = [cell for cell in plan["cells"] if cell["status"] != "planned"]
    blocked_lines = [
        "# Blocked Semantic Matrix Cells",
        "",
        "| Cell | System | Workflow | Reason | Evidence |",
        "|---|---|---|---|---|",
    ]
    for cell in blocked:
        blocked_lines.append(
            "| {name} | {system} | {workflow} | {reason} | {evidence} |".format(
                name=cell["name"],
                system=cell["system"],
                workflow=cell["workflow"],
                reason=cell["blocked_reason"],
                evidence=", ".join(cell["evidence"]),
            )
        )
    (output_dir / "blocked.md").write_text("\n".join(blocked_lines) + "\n", encoding="utf-8")

    total_runs = sum(len(cell["runs"]) for cell in plan["cells"])
    planned_cells = sum(1 for cell in plan["cells"] if cell["status"] == "planned")
    lines = [
        "# Semantic Benchmark Matrix Plan",
        "",
        f"Timestamp: {plan['timestamp']}",
        "",
        f"- planned_cells: {planned_cells}",
        f"- blocked_cells: {len(blocked)}",
        f"- planned_runner_commands: {len(plan['commands'])}",
        f"- planned_metric_rows_before_trial_expansion: {total_runs}",
        "",
        "| Cell | System | Workflow | Semantic Class | Status | Sizes |",
        "|---|---|---|---|---|---|",
    ]
    for cell in plan["cells"]:
        sizes = ", ".join(str(run["message_size_bytes"]) for run in cell["runs"]) or "blocked"
        lines.append(
            "| {name} | {system} | {workflow} | {semantic} | {status} | {sizes} |".format(
                name=cell["name"],
                system=cell["system"],
                workflow=cell["workflow"],
                semantic=cell["semantic_class"],
                status=cell["status"],
                sizes=sizes,
            )
        )
    lines.extend(
        [
            "",
            "Decision:",
            "",
            "- This plan pairs message size and operation count instead of cross-producting them.",
            "- Storage-backed Mofka no-wait and deferred-wait append cells are planned when validated; per-event wait/flush and storage catch-up cells remain blocked until they pass service-health and metrics gates.",
            "- The generated commands use the shared matrix runner so summary rows inherit the common metrics schema and service-health gates.",
        ]
    )
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--matrix-result-root", default=".agent/results/semantic-matrix-runs")
    parser.add_argument("--nodes", type=int, default=4)
    parser.add_argument("--clients", type=int, default=4)
    parser.add_argument("--size-operations", default="1024:10000,65536:2500")
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--partition", default="debug")
    parser.add_argument("--nodelist", default="")
    parser.add_argument("--slurm-time", default="00:30:00")
    parser.add_argument("--dry-run-commands", action="store_true")
    parser.add_argument(
        "--include-blocked-storage-mofka",
        action="store_true",
        help="Emit Mofka storage-backed commands even though the current fixed-baseline service is blocked.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = REPO_ROOT / args.output_dir
    plan = build_plan(args)
    write_outputs(plan, output_dir)
    print(output_dir.relative_to(REPO_ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
