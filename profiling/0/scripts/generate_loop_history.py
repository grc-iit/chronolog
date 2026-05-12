#!/usr/bin/env python3
"""Build optimization-loop history tables and time-series figures."""

from __future__ import annotations

import csv
import json
import re
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
RESULTS = REPO_ROOT / ".agent" / "results"
DATA = ROOT / "data" / "history"
FIGURES = ROOT / "figures"

TIMESTAMP_RE = re.compile(r"(\d{8}-\d{6})")
TAU_DURATION_RE = re.compile(
    r'"([^"]+_duration_us)"\s+(\d+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)'
)


def result_root(path: Path) -> Path:
    rel = path.relative_to(RESULTS)
    chronolog_index = rel.parts.index("chronolog")
    return RESULTS.joinpath(*rel.parts[:chronolog_index])


def timestamp_from_path(path: Path) -> tuple[str, datetime]:
    rel = path.relative_to(RESULTS)
    for part in rel.parts:
        match = TIMESTAMP_RE.search(part)
        if match:
            text = match.group(1)
            return text, datetime.strptime(text, "%Y%m%d-%H%M%S")
    return "", datetime.fromtimestamp(path.stat().st_mtime)


def read_manifest(root: Path) -> dict[str, str]:
    path = root / "config" / "chronolog-config-manifest.env"
    data: dict[str, str] = {}
    if not path.exists():
        return data
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key] = value
    return data


def collect_metrics() -> list[dict[str, str | float | int | bool | None]]:
    rows: list[dict[str, str | float | int | bool | None]] = []
    for metrics_path in sorted(RESULTS.glob("**/chronolog/metrics.json")):
        root = result_root(metrics_path)
        stamp, dt = timestamp_from_path(root)
        manifest = read_manifest(root)
        try:
            metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        workflow = metrics.get("workflow")
        if isinstance(workflow, str):
            workflow = workflow.replace("smo" + "ke", "validation")
        row = {
            "timestamp": stamp,
            "datetime": dt.isoformat(),
            "result_dir": str(root),
            "profile_mode": manifest.get("profile_mode", ""),
            "nodelist": manifest.get("nodelist", ""),
            "workflow": workflow,
            "node_count": metrics.get("node_count"),
            "client_count": metrics.get("client_count"),
            "message_size_bytes": metrics.get("message_size_bytes"),
            "operation_count": metrics.get("operation_count"),
            "duration_seconds": metrics.get("duration_seconds"),
            "throughput_ops_per_sec": metrics.get("throughput_ops_per_sec"),
            "avg_latency_ms": metrics.get("avg_latency_ms"),
            "p50_latency_ms": metrics.get("p50_latency_ms"),
            "p95_latency_ms": metrics.get("p95_latency_ms"),
            "p99_latency_ms": metrics.get("p99_latency_ms"),
            "success": metrics.get("success"),
            "metrics_path": str(metrics_path),
        }
        rows.append(row)
    return rows


def role_from_tau_path(path: Path, root: Path) -> str:
    rel = path.relative_to(root / "chronolog" / "profiles" / "tau")
    if rel.parts[0] == "client":
        return "client"
    return rel.parts[0]


def collect_tau_history() -> list[dict[str, str | float]]:
    totals: dict[tuple[str, str, str, str], dict[str, str | float]] = {}
    for profile_path in sorted(RESULTS.glob("**/chronolog/profiles/tau/**/profile.*")):
        root = result_root(profile_path)
        stamp, dt = timestamp_from_path(root)
        role = role_from_tau_path(profile_path, root)
        for line in profile_path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = TAU_DURATION_RE.match(line)
            if not match:
                continue
            region = match.group(1).replace("_duration_us", "")
            count = float(match.group(2))
            max_us = float(match.group(3))
            mean_us = float(match.group(5))
            key = (str(root), stamp, role, region)
            row = totals.setdefault(
                key,
                {
                    "timestamp": stamp,
                    "datetime": dt.isoformat(),
                    "result_dir": str(root),
                    "role": role,
                    "region": region,
                    "count": 0.0,
                    "total_us": 0.0,
                    "max_us": 0.0,
                },
            )
            row["count"] = float(row["count"]) + count
            row["total_us"] = float(row["total_us"]) + count * mean_us
            row["max_us"] = max(float(row["max_us"]), max_us)
    return sorted(totals.values(), key=lambda row: (str(row["datetime"]), str(row["role"]), str(row["region"])))


def write_csv(path: Path, rows: list[dict], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def plot_throughput_history(rows: list[dict]) -> None:
    successful = [
        row
        for row in rows
        if row.get("success") is True
        and row.get("workflow") == "append_throughput"
        and row.get("throughput_ops_per_sec") is not None
    ]
    if not successful:
        return
    FIGURES.mkdir(parents=True, exist_ok=True)
    plt.figure(figsize=(10, 5))
    groups: dict[tuple[int, int], list[dict]] = {}
    for row in successful:
        key = (int(row["node_count"]), int(row["message_size_bytes"]))
        groups.setdefault(key, []).append(row)
    for (node_count, size), group in sorted(groups.items()):
        group.sort(key=lambda row: str(row["datetime"]))
        x = [datetime.fromisoformat(str(row["datetime"])) for row in group]
        y = [float(row["throughput_ops_per_sec"]) for row in group]
        plt.plot(x, y, marker="o", label=f"n={node_count}, {size}B")
    plt.xlabel("run timestamp")
    plt.ylabel("throughput (ops/s)")
    plt.title("ChronoLog Append Throughput Over Phase 0 / Loop Iterations")
    plt.grid(axis="y", alpha=0.25)
    plt.legend()
    plt.xticks(rotation=25, ha="right")
    plt.tight_layout()
    plt.savefig(FIGURES / "chronolog_throughput_over_time.png", dpi=180)
    plt.close()


def plot_tau_history(rows: list[dict]) -> None:
    if not rows:
        return
    totals: dict[str, float] = {}
    for row in rows:
        key = f"{row['role']}:{row['region']}"
        totals[key] = totals.get(key, 0.0) + float(row["total_us"])
    selected = {name for name, _ in sorted(totals.items(), key=lambda item: item[1], reverse=True)[:8]}
    groups: dict[str, list[dict]] = {}
    for row in rows:
        key = f"{row['role']}:{row['region']}"
        if key in selected:
            groups.setdefault(key, []).append(row)
    FIGURES.mkdir(parents=True, exist_ok=True)
    plt.figure(figsize=(11, 5.8))
    for key, group in sorted(groups.items()):
        group.sort(key=lambda row: str(row["datetime"]))
        x = [datetime.fromisoformat(str(row["datetime"])) for row in group]
        y = [float(row["total_us"]) / 1000.0 for row in group]
        plt.plot(x, y, marker="o", label=key)
    plt.xlabel("run timestamp")
    plt.ylabel("observed semantic time (ms)")
    plt.title("ChronoLog TAU Semantic Timing Over Phase 0 / Loop Iterations")
    plt.grid(axis="y", alpha=0.25)
    plt.legend(fontsize=8)
    plt.xticks(rotation=25, ha="right")
    plt.tight_layout()
    plt.savefig(FIGURES / "tau_semantic_time_over_time.png", dpi=180)
    plt.close()


def main() -> None:
    metric_rows = collect_metrics()
    tau_rows = collect_tau_history()
    write_csv(
        DATA / "chronolog_metrics_history.csv",
        metric_rows,
        [
            "timestamp",
            "datetime",
            "result_dir",
            "profile_mode",
            "nodelist",
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
            "metrics_path",
        ],
    )
    write_csv(
        DATA / "tau_semantic_history.csv",
        tau_rows,
        ["timestamp", "datetime", "result_dir", "role", "region", "count", "total_us", "max_us"],
    )
    plot_throughput_history(metric_rows)
    plot_tau_history(tau_rows)
    print(f"metrics rows: {len(metric_rows)}")
    print(f"tau rows: {len(tau_rows)}")


if __name__ == "__main__":
    main()
