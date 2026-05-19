#!/usr/bin/env python3

import argparse
import json
import sys
from pathlib import Path


def as_int(value, default=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


def backfill(path: Path) -> bool:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"{path}: cannot parse JSON: {exc}", file=sys.stderr)
        return False

    changed = False

    operation_count = as_int(data.get("operation_count"), 0)
    node_count = as_int(data.get("node_count"), 0)
    client_count = as_int(data.get("client_count"), 1)
    message_size = as_int(data.get("message_size_bytes"), 0)

    operation_count_per_client = as_int(data.get("operation_count_per_client"), operation_count)
    total_operation_count = as_int(
        data.get("total_operation_count"),
        operation_count_per_client * client_count,
    )
    total_payload_bytes = as_int(
        data.get("total_payload_bytes"),
        total_operation_count * message_size,
    )

    defaults = {
        "operation_count_per_client": operation_count_per_client,
        "message_count_per_client": operation_count_per_client,
        "messages_per_client": operation_count_per_client,
        "total_operation_count": total_operation_count,
        "total_message_count": total_operation_count,
        "total_messages": total_operation_count,
        "parallel_client_count": client_count,
        "parallel_clients": client_count,
        "nodes": node_count,
        "total_payload_bytes": total_payload_bytes,
    }
    for key, value in defaults.items():
        if key not in data:
            data[key] = value
            changed = True

    if changed:
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return changed


def main() -> int:
    parser = argparse.ArgumentParser(description="Backfill explicit workload dimension aliases in metrics.json files.")
    parser.add_argument("metrics", nargs="+", type=Path)
    args = parser.parse_args()

    changed = 0
    for metrics_path in args.metrics:
        if backfill(metrics_path):
            changed += 1
    print(f"backfilled {changed} metrics files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
