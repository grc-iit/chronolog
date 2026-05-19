#!/usr/bin/env python3
"""Summarize ChronoLog Keeper packed-tail bulk transfer attribution.

The benchmark harness stores raw per-RPC KeeperTailBulkStats rows inside
metrics.json. This helper turns those rows into per-Keeper and global summaries
so packed-tail bottleneck decisions are based on skew and transfer/read cost,
not only aggregate averages.
"""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path
from typing import Any


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * pct / 100.0
    lower = int(rank)
    upper = min(lower + 1, len(ordered) - 1)
    weight = rank - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def mb_per_sec(bytes_count: float, micros: float) -> float:
    if micros <= 0:
        return 0.0
    return (bytes_count / (1024.0 * 1024.0)) / (micros / 1_000_000.0)


def log_name(row: dict[str, Any]) -> str:
    raw = str(row.get("log", "unknown"))
    return Path(raw).name if raw else "unknown"


def summarize_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    by_keeper: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        by_keeper[log_name(row)].append(row)

    per_keeper = []
    for keeper_log, keeper_rows in sorted(by_keeper.items()):
        payload_bytes = sum(float(row.get("payload_bytes", 0) or 0) for row in keeper_rows)
        event_count = sum(float(row.get("event_count", 0) or 0) for row in keeper_rows)
        transfer_us_values = [float(row.get("bulk_transfer_us", 0) or 0) for row in keeper_rows]
        collect_us_values = [float(row.get("collect_us", 0) or 0) for row in keeper_rows]
        total_us_values = [float(row.get("total_us", 0) or 0) for row in keeper_rows]
        transfer_us = sum(transfer_us_values)
        collect_us = sum(collect_us_values)
        total_us = sum(total_us_values)
        per_keeper.append(
            {
                "keeper_log": keeper_log,
                "rpc_count": len(keeper_rows),
                "event_count": int(event_count),
                "payload_bytes": int(payload_bytes),
                "bulk_transfer_us_total": transfer_us,
                "bulk_transfer_us_avg": transfer_us / len(keeper_rows) if keeper_rows else 0.0,
                "bulk_transfer_us_p95": percentile(transfer_us_values, 95),
                "bulk_transfer_us_max": max(transfer_us_values) if transfer_us_values else 0.0,
                "bulk_transfer_payload_mb_per_sec": mb_per_sec(payload_bytes, transfer_us),
                "collect_us_total": collect_us,
                "collect_us_avg": collect_us / len(keeper_rows) if keeper_rows else 0.0,
                "collect_us_p95": percentile(collect_us_values, 95),
                "total_us_total": total_us,
                "total_us_avg": total_us / len(keeper_rows) if keeper_rows else 0.0,
                "total_us_p95": percentile(total_us_values, 95),
                "total_us_max": max(total_us_values) if total_us_values else 0.0,
            }
        )

    payload_mbps = [row["bulk_transfer_payload_mb_per_sec"] for row in per_keeper]
    total_us_values = [float(row.get("total_us", 0) or 0) for row in rows]
    transfer_us_values = [float(row.get("bulk_transfer_us", 0) or 0) for row in rows]
    collect_us_values = [float(row.get("collect_us", 0) or 0) for row in rows]
    payload_bytes = sum(float(row.get("payload_bytes", 0) or 0) for row in rows)
    transfer_us = sum(transfer_us_values)

    slowest_transfer = max(per_keeper, key=lambda row: row["bulk_transfer_us_total"], default={})
    slowest_total = max(per_keeper, key=lambda row: row["total_us_total"], default={})
    fastest_mbps = min(payload_mbps) if payload_mbps else 0.0
    slowest_mbps = max(payload_mbps) if payload_mbps else 0.0

    return {
        "summary": {
            "keeper_count": len(per_keeper),
            "rpc_count": len(rows),
            "event_count": int(sum(float(row.get("event_count", 0) or 0) for row in rows)),
            "payload_bytes": int(payload_bytes),
            "bulk_transfer_us_total": transfer_us,
            "bulk_transfer_us_avg": sum(transfer_us_values) / len(rows) if rows else 0.0,
            "bulk_transfer_us_p95": percentile(transfer_us_values, 95),
            "bulk_transfer_us_p99": percentile(transfer_us_values, 99),
            "collect_us_avg": sum(collect_us_values) / len(rows) if rows else 0.0,
            "collect_us_p95": percentile(collect_us_values, 95),
            "total_us_avg": sum(total_us_values) / len(rows) if rows else 0.0,
            "total_us_p95": percentile(total_us_values, 95),
            "payload_mb_per_sec": mb_per_sec(payload_bytes, transfer_us),
            "per_keeper_min_payload_mb_per_sec": fastest_mbps,
            "per_keeper_max_payload_mb_per_sec": slowest_mbps,
            "per_keeper_payload_mbps_ratio": (slowest_mbps / fastest_mbps) if fastest_mbps > 0 else 0.0,
            "slowest_transfer_keeper_log": slowest_transfer.get("keeper_log", ""),
            "slowest_total_keeper_log": slowest_total.get("keeper_log", ""),
        },
        "per_keeper": per_keeper,
    }


def load_rows(metrics_path: Path) -> list[dict[str, Any]]:
    metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
    stats = metrics.get("keeper_tail_bulk_stats", {})
    rows = stats.get("per_rpc", [])
    if not isinstance(rows, list):
        raise SystemExit(f"{metrics_path}: keeper_tail_bulk_stats.per_rpc is not a list")
    return [row for row in rows if isinstance(row, dict)]


def write_markdown(summary: dict[str, Any], metrics_path: Path, output: Path) -> None:
    top = summary["summary"]
    lines = [
        "# Keeper Tail Bulk Attribution",
        "",
        f"Metrics: `{metrics_path}`",
        "",
        "## Summary",
        "",
        f"- Keepers: `{top['keeper_count']}`",
        f"- RPCs: `{top['rpc_count']}`",
        f"- Events: `{top['event_count']}`",
        f"- Payload bytes: `{top['payload_bytes']}`",
        f"- Aggregate payload throughput: `{top['payload_mb_per_sec']:.3f} MiB/s`",
        f"- Bulk transfer avg/p95/p99: `{top['bulk_transfer_us_avg']:.3f} / "
        f"{top['bulk_transfer_us_p95']:.3f} / {top['bulk_transfer_us_p99']:.3f} us`",
        f"- Collect avg/p95: `{top['collect_us_avg']:.3f} / {top['collect_us_p95']:.3f} us`",
        f"- Total avg/p95: `{top['total_us_avg']:.3f} / {top['total_us_p95']:.3f} us`",
        f"- Per-Keeper payload throughput min/max/ratio: "
        f"`{top['per_keeper_min_payload_mb_per_sec']:.3f} / "
        f"{top['per_keeper_max_payload_mb_per_sec']:.3f} / "
        f"{top['per_keeper_payload_mbps_ratio']:.3f}`",
        f"- Slowest transfer Keeper: `{top['slowest_transfer_keeper_log']}`",
        f"- Slowest total Keeper: `{top['slowest_total_keeper_log']}`",
        "",
        "## Per Keeper",
        "",
        "| Keeper log | RPCs | Events | Payload bytes | Transfer MiB/s | Transfer avg us | Transfer p95 us | Total avg us | Total p95 us |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in summary["per_keeper"]:
        lines.append(
            f"| `{row['keeper_log']}` | {row['rpc_count']} | {row['event_count']} | "
            f"{row['payload_bytes']} | {row['bulk_transfer_payload_mb_per_sec']:.3f} | "
            f"{row['bulk_transfer_us_avg']:.3f} | {row['bulk_transfer_us_p95']:.3f} | "
            f"{row['total_us_avg']:.3f} | {row['total_us_p95']:.3f} |"
        )
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metrics_json", type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--md-out", type=Path)
    args = parser.parse_args()

    rows = load_rows(args.metrics_json)
    summary = summarize_rows(rows)

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.md_out:
        args.md_out.parent.mkdir(parents=True, exist_ok=True)
        write_markdown(summary, args.metrics_json, args.md_out)
    if not args.json_out and not args.md_out:
        print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
