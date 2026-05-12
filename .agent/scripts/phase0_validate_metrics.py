#!/usr/bin/env python3

import argparse
import json
import sys
from pathlib import Path


REQUIRED_FIELDS = {
    "system": str,
    "workflow": str,
    "node_count": (int, float),
    "client_count": (int, float),
    "message_size_bytes": (int, float),
    "operation_count": (int, float),
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
