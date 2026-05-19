#!/usr/bin/env python3
"""Verify ChronoLog archive/range A/B rows are comparable before judging performance."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


REQUIRED_EQUAL_FIELDS = [
    "system",
    "workflow",
    "semantic_boundary",
    "append_ack_boundary",
    "durability_boundary",
    "read_path",
    "storage_backend",
    "node_count",
    "client_count",
    "operation_count",
    "message_size_bytes",
    "chronicle_count",
    "story_count",
    "chronolog_hdf5_archive_atomic_rename",
    "chronolog_hdf5_archive_layout",
    "chronolog_raw_blob_async_close",
    "chronolog_raw_blob_async_publish",
    "chronolog_raw_blob_async_publish_threads",
    "chronolog_raw_blob_publish_before_close",
    "chronolog_raw_blob_sidecar_meta",
    "keeper_stop_story_flush_drain",
    "visor_parallel_keeper_stop",
    "visor_parallel_recording_stop",
    "grapher_stop_drain_complete_wait",
    "grapher_stop_drain_complete_wait_async",
    "grapher_stop_drain_wait_outside_lock",
]

REQUIRED_SUCCESS_FIELDS = [
    "success",
    "archive_event_count",
    "readback_event_count",
    "grapher_orphan_chunk_count",
    "keeper_orphan_warning_count",
]

IDENTITY_FIELDS = [
    "chronolog_install_dir",
    "chronolog_bin_dir",
    "chronolog_deploy_script",
    "chronolog_benchmark_bin",
]


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def stage_identity(metrics_path: Path) -> dict[str, str]:
    stage_path = metrics_path.parent.parent / "stage-attribution" / "archive-stage-attribution.json"
    if not stage_path.exists():
        return {}
    data = load_json(stage_path)
    rows = data.get("run_identity") or []
    return {str(row.get("metric")): str(row.get("value")) for row in rows if row.get("metric") and row.get("value")}


def identity(metrics_path: Path, metrics: dict[str, Any]) -> dict[str, str]:
    result = {field: str(metrics.get(field)) for field in IDENTITY_FIELDS if metrics.get(field)}
    fallback = stage_identity(metrics_path)
    for field in IDENTITY_FIELDS:
        if field not in result and fallback.get(field):
            result[field] = fallback[field]
    return result


def metric_int(metrics: dict[str, Any], field: str) -> int | None:
    value = metrics.get(field)
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def profile_nonempty_count(metrics: dict[str, Any]) -> int | None:
    return metric_int(metrics, "chronolog_profile_nonempty_artifact_count")


def compare(args: argparse.Namespace) -> tuple[bool, dict[str, Any]]:
    control_path = args.control.resolve()
    candidate_path = args.candidate.resolve()
    control = load_json(control_path)
    candidate = load_json(candidate_path)
    allowed = set(args.allow_difference)

    failures: list[str] = []
    warnings: list[str] = []
    checks: list[dict[str, Any]] = []

    if args.claim_level == "promotion":
        if args.min_node_count is None or args.min_node_count < 4:
            failures.append("promotion claim requires --min-node-count >= 4")
        if (
            args.min_operation_count is None
            and args.min_archive_event_count is None
            and args.min_data_bytes is None
        ):
            failures.append(
                "promotion claim requires at least one volume gate: --min-operation-count, "
                "--min-archive-event-count, or --min-data-bytes"
            )

    for label, metrics in [("control", control), ("candidate", candidate)]:
        if metrics.get("success") is not True:
            failures.append(f"{label}: success is not true")
        if metrics.get("grapher_orphan_chunk_count") != 0:
            failures.append(f"{label}: grapher_orphan_chunk_count is {metrics.get('grapher_orphan_chunk_count')}")
        if metrics.get("keeper_orphan_warning_count") != 0:
            failures.append(f"{label}: keeper_orphan_warning_count is {metrics.get('keeper_orphan_warning_count')}")
        for field in REQUIRED_SUCCESS_FIELDS:
            if field not in metrics:
                failures.append(f"{label}: missing required field {field}")
        if args.min_node_count is not None:
            node_count = metric_int(metrics, "node_count")
            if node_count is None or node_count < args.min_node_count:
                failures.append(
                    f"{label}: node_count {metrics.get('node_count')!r} is below required "
                    f"{args.min_node_count}"
                )
        if args.min_operation_count is not None:
            operation_count = metric_int(metrics, "operation_count")
            if operation_count is None or operation_count < args.min_operation_count:
                failures.append(
                    f"{label}: operation_count {metrics.get('operation_count')!r} is below required "
                    f"{args.min_operation_count}"
                )
        if args.min_archive_event_count is not None:
            archive_event_count = metric_int(metrics, "archive_event_count")
            if archive_event_count is None or archive_event_count < args.min_archive_event_count:
                failures.append(
                    f"{label}: archive_event_count {metrics.get('archive_event_count')!r} is below required "
                    f"{args.min_archive_event_count}"
                )
        if args.min_data_bytes is not None:
            archive_event_count = metric_int(metrics, "archive_event_count")
            message_size = metric_int(metrics, "message_size_bytes")
            data_bytes = None
            if archive_event_count is not None and message_size is not None:
                data_bytes = archive_event_count * message_size
            if data_bytes is None or data_bytes < args.min_data_bytes:
                failures.append(
                    f"{label}: derived archive data bytes {data_bytes!r} is below required "
                    f"{args.min_data_bytes}"
                )
        if args.require_profile_artifacts:
            profile_count = profile_nonempty_count(metrics)
            if metrics.get("chronolog_profile_valid") is not True:
                failures.append(f"{label}: chronolog_profile_valid is not true")
            if profile_count is None or profile_count < args.min_profile_artifacts:
                failures.append(
                    f"{label}: chronolog_profile_nonempty_artifact_count {profile_count!r} is below required "
                    f"{args.min_profile_artifacts}"
                )

    for field in REQUIRED_EQUAL_FIELDS:
        left = control.get(field)
        right = candidate.get(field)
        status = "pass"
        if left != right:
            status = "allowed" if field in allowed else "fail"
            if status == "fail":
                failures.append(f"{field}: control={left!r} candidate={right!r}")
        checks.append({"field": field, "control": left, "candidate": right, "status": status})

    for field in allowed:
        left = control.get(field)
        right = candidate.get(field)
        if left == right:
            warnings.append(f"allowed difference {field} did not differ")
        if field not in REQUIRED_EQUAL_FIELDS:
            checks.append({"field": field, "control": left, "candidate": right, "status": "allowed_difference"})

    control_identity = identity(control_path, control)
    candidate_identity = identity(candidate_path, candidate)
    if not control_identity:
        failures.append("control: missing run identity")
    if not candidate_identity:
        failures.append("candidate: missing run identity")
    for field in IDENTITY_FIELDS:
        left = control_identity.get(field)
        right = candidate_identity.get(field)
        status = "pass" if left and right and left == right else "fail"
        if status == "fail":
            failures.append(f"run identity {field}: control={left!r} candidate={right!r}")
        checks.append({"field": field, "control": left, "candidate": right, "status": status})

    if control.get("archive_event_count") != candidate.get("archive_event_count"):
        failures.append(
            "archive_event_count mismatch: "
            f"control={control.get('archive_event_count')!r} candidate={candidate.get('archive_event_count')!r}"
        )
    if control.get("readback_event_count") != candidate.get("readback_event_count"):
        failures.append(
            "readback_event_count mismatch: "
            f"control={control.get('readback_event_count')!r} candidate={candidate.get('readback_event_count')!r}"
        )

    report = {
        "control": str(control_path),
        "candidate": str(candidate_path),
        "claim_level": args.claim_level,
        "allowed_differences": sorted(allowed),
        "min_node_count": args.min_node_count,
        "min_operation_count": args.min_operation_count,
        "min_archive_event_count": args.min_archive_event_count,
        "min_data_bytes": args.min_data_bytes,
        "require_profile_artifacts": args.require_profile_artifacts,
        "min_profile_artifacts": args.min_profile_artifacts,
        "passed": not failures,
        "failures": failures,
        "warnings": warnings,
        "checks": checks,
    }
    return not failures, report


def write_report(report: dict[str, Any], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "archive-ab-verify.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")

    md = [
        "# Archive A/B Verification",
        "",
        f"- control: `{report['control']}`",
        f"- candidate: `{report['candidate']}`",
        f"- claim_level: `{report['claim_level']}`",
        f"- passed: `{report['passed']}`",
        f"- allowed differences: `{', '.join(report['allowed_differences'])}`",
        f"- min_node_count: `{report['min_node_count']}`",
        f"- min_operation_count: `{report['min_operation_count']}`",
        f"- min_archive_event_count: `{report['min_archive_event_count']}`",
        f"- min_data_bytes: `{report['min_data_bytes']}`",
        f"- require_profile_artifacts: `{report['require_profile_artifacts']}`",
        f"- min_profile_artifacts: `{report['min_profile_artifacts']}`",
        "",
    ]
    if report["failures"]:
        md.extend(["## Failures", ""])
        md.extend(f"- {failure}" for failure in report["failures"])
        md.append("")
    if report["warnings"]:
        md.extend(["## Warnings", ""])
        md.extend(f"- {warning}" for warning in report["warnings"])
        md.append("")
    md.extend(["## Checks", "", "| Field | Control | Candidate | Status |", "|---|---|---|---|"])
    for row in report["checks"]:
        md.append(
            f"| `{row['field']}` | `{row.get('control')}` | `{row.get('candidate')}` | `{row['status']}` |"
        )
    (output_dir / "archive-ab-verify.md").write_text("\n".join(md) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--control", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--claim-level", choices=["fallback", "promotion"], default="fallback")
    parser.add_argument("--allow-difference", action="append", default=[])
    parser.add_argument("--min-node-count", type=int)
    parser.add_argument("--min-operation-count", type=int)
    parser.add_argument("--min-archive-event-count", type=int)
    parser.add_argument("--min-data-bytes", type=int)
    parser.add_argument("--require-profile-artifacts", action="store_true")
    parser.add_argument("--min-profile-artifacts", type=int, default=1)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    passed, report = compare(args)
    if args.output_dir:
        write_report(report, args.output_dir)
    else:
        print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
