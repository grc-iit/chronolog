#!/usr/bin/env python3

import argparse
import json
import sys
from pathlib import Path


REQUIRED_FIELDS = {
    "system": str,
    "workflow": str,
    "node_count": (int, float),
    "nodes": (int, float),
    "client_count": (int, float),
    "parallel_clients": (int, float),
    "message_size_bytes": (int, float),
    "operation_count": (int, float),
    "operation_count_per_client": (int, float),
    "message_count_per_client": (int, float),
    "messages_per_client": (int, float),
    "total_operation_count": (int, float),
    "total_message_count": (int, float),
    "total_messages": (int, float),
    "total_payload_bytes": (int, float),
    "parallel_client_count": (int, float),
    "duration_seconds": (int, float),
    "throughput_ops_per_sec": (int, float),
    "avg_latency_ms": (int, float, type(None)),
    "p50_latency_ms": (int, float, type(None)),
    "p95_latency_ms": (int, float, type(None)),
    "p99_latency_ms": (int, float, type(None)),
    "success": bool,
}

ALLOWED_SYSTEMS = {"chronolog", "kafka", "mofka"}


def validate_metrics(path):
    errors = []
    try:
        data = json.loads(path.read_text())
    except Exception as exc:
        return [f"{path}: cannot parse JSON: {exc}"]

    for field, expected_type in REQUIRED_FIELDS.items():
        if field not in data:
            errors.append(f"{path}: missing required field {field}")
            continue
        if not isinstance(data[field], expected_type):
            errors.append(f"{path}: field {field} has invalid type {type(data[field]).__name__}")

    if data.get("system") not in ALLOWED_SYSTEMS:
        errors.append(f"{path}: unsupported system {data.get('system')!r}")
    if data.get("duration_seconds", 0) < 0:
        errors.append(f"{path}: duration_seconds must be non-negative")
    if data.get("throughput_ops_per_sec", 0) < 0:
        errors.append(f"{path}: throughput_ops_per_sec must be non-negative")
    count_fields = (
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
    )
    for field in count_fields:
        value = data.get(field)
        if isinstance(value, (int, float)) and value < 0:
            errors.append(f"{path}: {field} must be non-negative")
        if data.get("success") is True and isinstance(value, (int, float)) and value <= 0:
            errors.append(f"{path}: {field} must be positive for successful benchmark metrics")
    if (
        isinstance(data.get("operation_count_per_client"), (int, float))
        and isinstance(data.get("client_count"), (int, float))
    ):
        expected_total = int(data["operation_count_per_client"]) * int(data["client_count"])
        actual_total = int(data.get("total_operation_count") or 0)
        if actual_total != expected_total:
            errors.append(
                f"{path}: total_operation_count {actual_total} does not match "
                f"operation_count_per_client*client_count {expected_total}"
            )
    if (
        isinstance(data.get("operation_count"), (int, float))
        and isinstance(data.get("operation_count_per_client"), (int, float))
        and int(data["operation_count"]) != int(data["operation_count_per_client"])
    ):
        errors.append(
            f"{path}: operation_count {data['operation_count']} does not match "
            f"operation_count_per_client {data['operation_count_per_client']}"
        )
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
        if (
            isinstance(data.get(canonical), (int, float))
            and isinstance(data.get(alias), (int, float))
            and int(data[canonical]) != int(data[alias])
        ):
            errors.append(f"{path}: {alias} {data[alias]} does not match {canonical} {data[canonical]}")
    if (
        isinstance(data.get("message_size_bytes"), (int, float))
        and isinstance(data.get("total_operation_count"), (int, float))
        and isinstance(data.get("total_payload_bytes"), (int, float))
    ):
        expected_payload = int(data["message_size_bytes"]) * int(data["total_operation_count"])
        actual_payload = int(data["total_payload_bytes"])
        if actual_payload != expected_payload:
            errors.append(
                f"{path}: total_payload_bytes {actual_payload} does not match "
                f"message_size_bytes*total_operation_count {expected_payload}"
            )

    return errors


def main():
    parser = argparse.ArgumentParser(description="Validate Phase 0 metrics.json files.")
    parser.add_argument("metrics", nargs="+", type=Path)
    args = parser.parse_args()

    errors = []
    for metrics_path in args.metrics:
        errors.extend(validate_metrics(metrics_path))

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    for metrics_path in args.metrics:
        print(f"valid {metrics_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
