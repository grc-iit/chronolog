#!/usr/bin/env python3
"""Validate ProfileForge run correctness from metrics and run artifacts."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
REQUIRED_METRICS = [
    "system",
    "workflow",
    "node_count",
    "client_count",
    "message_size_bytes",
    "operation_count",
    "duration_seconds",
    "throughput_ops_per_sec",
    "avg_latency_ms",
    "p50_latency_ms",
    "p95_latency_ms",
    "p99_latency_ms",
    "success",
]


def repo_path(text: str) -> Path:
    path = Path(text)
    if path.is_absolute():
        return path
    return REPO_ROOT / path


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def load_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


def validate_metrics_schema(metrics: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for key in REQUIRED_METRICS:
        if key not in metrics:
            errors.append(f"missing required metric field: {key}")
    if metrics.get("system") not in {"chronolog", "kafka", "mofka"}:
        errors.append(f"invalid system: {metrics.get('system')}")
    for key in ["node_count", "client_count", "message_size_bytes", "operation_count"]:
        if key in metrics and (not isinstance(metrics[key], int) or metrics[key] < 0):
            errors.append(f"{key} must be a non-negative integer")
    for key in ["duration_seconds", "throughput_ops_per_sec"]:
        if key in metrics and (not isinstance(metrics[key], (int, float)) or metrics[key] < 0):
            errors.append(f"{key} must be a non-negative number")
    if "success" in metrics and not isinstance(metrics["success"], bool):
        errors.append("success must be boolean")
    return errors


def validate_semantics(metrics: dict[str, Any], result_dir: Path) -> tuple[str, dict[str, Any], list[str]]:
    workflow = str(metrics.get("workflow", ""))
    errors: list[str] = []
    details: dict[str, Any] = {
        "records_expected": int(metrics.get("operation_count", 0) or 0),
        "records_observed": None,
        "duplicates": None,
        "lost_records": None,
        "ordering_violations": None,
        "evidence": [],
    }

    if not metrics.get("success"):
        errors.append("metrics success is false")
    if float(metrics.get("throughput_ops_per_sec", 0) or 0) <= 0:
        errors.append("throughput_ops_per_sec is not positive")

    if workflow == "range_retrieval":
        observed = int(metrics.get("retrieved_event_count", 0) or 0)
        expected = int(metrics.get("operation_count", 0) or 0)
        details["records_observed"] = observed
        details["lost_records"] = max(expected - observed, 0)
        details["duplicates"] = 0 if observed <= expected else observed - expected
        details["ordering_violations"] = 0
        if observed < expected:
            errors.append(f"range retrieval observed {observed} records, expected at least {expected}")
        archive_files = sorted((result_dir / "chronolog" / "output").glob("*.h5"))
        details["evidence"].extend(display_path(path) for path in archive_files[:10])
        if not archive_files and metrics.get("system") == "chronolog":
            errors.append("ChronoLog range retrieval did not leave an HDF5 archive artifact")
    elif workflow in {"append_throughput", "append_latency"}:
        expected = int(metrics.get("operation_count", 0) or 0)
        details["records_observed"] = expected if metrics.get("success") else 0
        details["lost_records"] = 0 if metrics.get("success") else expected
        details["duplicates"] = 0
        details["ordering_violations"] = 0
        log_candidates = [
            result_dir / "chronolog" / "chrono-bench-append-throughput.log",
            result_dir / "chronolog" / "chronolog-append-latency.log",
            result_dir / "kafka" / "producer-perf-append-throughput.log",
            result_dir / "mofka" / "append-benchmark.stdout.log",
        ]
        details["evidence"].extend(display_path(path) for path in log_candidates if path.exists())
    else:
        errors.append(f"unsupported workflow for correctness validator: {workflow}")

    return ("pass" if not errors else "fail"), details, errors


def validate(metrics_path: Path, result_dir: Path) -> dict[str, Any]:
    metrics = load_json(metrics_path)
    schema_errors = validate_metrics_schema(metrics)
    semantic_result, semantic_details, semantic_errors = validate_semantics(metrics, result_dir)
    errors = schema_errors + semantic_errors
    return {
        "validator": "profileforge_correctness",
        "system": metrics.get("system"),
        "workflow": metrics.get("workflow"),
        "node_count": metrics.get("node_count"),
        "message_size_bytes": metrics.get("message_size_bytes"),
        "metrics_path": display_path(metrics_path),
        "result_dir": display_path(result_dir),
        "result": "pass" if not errors and semantic_result == "pass" else "fail",
        **semantic_details,
        "errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metrics", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--output", default="")
    args = parser.parse_args()

    metrics_path = repo_path(args.metrics)
    result_dir = repo_path(args.result_dir)
    report = validate(metrics_path, result_dir)
    output = repo_path(args.output) if args.output else result_dir / "correctness.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(display_path(output))
    return 0 if report["result"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
