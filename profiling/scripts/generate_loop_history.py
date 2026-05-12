#!/usr/bin/env python3
"""Build explicit ProfileForge loop-history tables and figures.

The top-level profiling history is intentionally sparse. It is not a
validation-run scraper. New loop iterations are added by editing
data/history/iteration_map.csv, then running this script.
"""

from __future__ import annotations

import csv
import json
import re
from datetime import datetime
from pathlib import Path

import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parent
DATA = ROOT / "data" / "history"
FIGURES = ROOT / "figures"

ITERATION_MAP = DATA / "iteration_map.csv"
FIXED_BASELINES = DATA / "fixed_baselines.csv"

TAU_DURATION_RE = re.compile(
    r'"([^"]+_duration_us)"\s+(\d+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)'
)


def repo_path(text: str) -> Path:
    path = Path(text)
    if path.is_absolute():
        return path
    return REPO_ROOT / path


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise SystemExit(f"missing required input: {path}")
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def read_json(path: Path) -> dict:
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


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


def baseline_key(system: str, workflow: str, node_count: int, message_size: int) -> tuple[str, str, int, int]:
    return (system, workflow, node_count, message_size)


def load_baselines() -> dict[tuple[str, str, int, int], dict[str, str | float]]:
    baselines: dict[tuple[str, str, int, int], dict[str, str | float]] = {}
    for row in read_csv(FIXED_BASELINES):
        metrics_path = repo_path(row["metrics_path"])
        metrics = read_json(metrics_path)
        key = baseline_key(
            row["system"],
            row["workflow"],
            int(row["node_count"]),
            int(row["message_size_bytes"]),
        )
        baselines[key] = {
            **row,
            "throughput_ops_per_sec": float(metrics["throughput_ops_per_sec"]),
            "operation_count": str(metrics.get("operation_count", "")),
            "metrics_path": row["metrics_path"],
        }
    return baselines


def collect_iteration_metrics() -> list[dict[str, str | float | int | bool | None]]:
    baselines = load_baselines()
    rows: list[dict[str, str | float | int | bool | None]] = []

    for item in read_csv(ITERATION_MAP):
        iteration = int(item["iteration"])
        root = repo_path(item["result_dir"])
        metrics_path = root / "chronolog" / "metrics.json"
        metrics = read_json(metrics_path)
        manifest = read_manifest(root)

        workflow = str(metrics["workflow"])
        node_count = int(metrics["node_count"])
        message_size = int(metrics["message_size_bytes"])
        throughput = float(metrics["throughput_ops_per_sec"])

        kafka = baselines.get(baseline_key("kafka", workflow, node_count, message_size))
        mofka = baselines.get(baseline_key("mofka", workflow, node_count, message_size))

        rows.append(
            {
                "iteration": iteration,
                "timestamp": item["timestamp"],
                "datetime": datetime.strptime(item["timestamp"], "%Y%m%d-%H%M%S").isoformat(),
                "label": item.get("label", ""),
                "commit": item.get("commit", ""),
                "result_dir": item["result_dir"],
                "profile_mode": manifest.get("profile_mode", ""),
                "nodelist": manifest.get("nodelist", ""),
                "workflow": workflow,
                "node_count": node_count,
                "client_count": metrics.get("client_count"),
                "message_size_bytes": message_size,
                "operation_count": metrics.get("operation_count"),
                "duration_seconds": metrics.get("duration_seconds"),
                "chronolog_throughput_ops_per_sec": throughput,
                "chronolog_throughput_ratio_to_iteration0": None,
                "kafka_baseline_throughput_ops_per_sec": kafka["throughput_ops_per_sec"] if kafka else None,
                "chronolog_to_kafka_throughput_ratio": throughput / float(kafka["throughput_ops_per_sec"]) if kafka else None,
                "mofka_baseline_throughput_ops_per_sec": mofka["throughput_ops_per_sec"] if mofka else None,
                "chronolog_to_mofka_throughput_ratio": throughput / float(mofka["throughput_ops_per_sec"]) if mofka else None,
                "success": metrics.get("success"),
                "metrics_path": str(metrics_path.relative_to(REPO_ROOT)),
                "kafka_baseline_metrics_path": kafka["metrics_path"] if kafka else "",
                "mofka_baseline_metrics_path": mofka["metrics_path"] if mofka else "",
                "baseline_note": item.get("baseline_note", ""),
            }
        )

    base_by_key: dict[tuple[str, int, int], float] = {}
    for row in sorted(rows, key=lambda r: int(r["iteration"])):
        key = (str(row["workflow"]), int(row["node_count"]), int(row["message_size_bytes"]))
        base_by_key.setdefault(key, float(row["chronolog_throughput_ops_per_sec"]))
        row["chronolog_throughput_ratio_to_iteration0"] = (
            float(row["chronolog_throughput_ops_per_sec"]) / base_by_key[key]
        )

    return sorted(rows, key=lambda r: int(r["iteration"]))


def role_from_tau_path(path: Path, root: Path) -> str:
    rel = path.relative_to(root / "chronolog" / "profiles" / "tau")
    if rel.parts[0] == "client":
        return "client"
    return rel.parts[0]


def collect_tau_history() -> list[dict[str, str | float | int]]:
    totals: dict[tuple[int, str, str], dict[str, str | float | int]] = {}
    for item in read_csv(ITERATION_MAP):
        iteration = int(item["iteration"])
        root = repo_path(item["result_dir"])
        for profile_path in sorted((root / "chronolog" / "profiles" / "tau").glob("**/profile.*")):
            role = role_from_tau_path(profile_path, root)
            for line in profile_path.read_text(encoding="utf-8", errors="replace").splitlines():
                match = TAU_DURATION_RE.match(line)
                if not match:
                    continue
                region = match.group(1).replace("_duration_us", "")
                count = float(match.group(2))
                max_us = float(match.group(3))
                mean_us = float(match.group(5))
                key = (iteration, role, region)
                row = totals.setdefault(
                    key,
                    {
                        "iteration": iteration,
                        "timestamp": item["timestamp"],
                        "datetime": datetime.strptime(item["timestamp"], "%Y%m%d-%H%M%S").isoformat(),
                        "label": item.get("label", ""),
                        "result_dir": item["result_dir"],
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

    return sorted(totals.values(), key=lambda row: (int(row["iteration"]), str(row["role"]), str(row["region"])))


def write_csv(path: Path, rows: list[dict], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def plot_throughput_history(rows: list[dict]) -> None:
    if not rows:
        return
    FIGURES.mkdir(parents=True, exist_ok=True)
    plt.figure(figsize=(8.5, 4.8))
    x = [int(row["iteration"]) for row in rows]
    y = [float(row["chronolog_throughput_ops_per_sec"]) for row in rows]
    plt.plot(x, y, marker="o", linewidth=2.0, label="ChronoLog")
    plt.xlabel("ProfileForge iteration")
    plt.ylabel("throughput (ops/s)")
    plt.title("ChronoLog Append Throughput by Optimization Iteration")
    plt.grid(axis="y", alpha=0.25)
    plt.xticks(x)
    plt.legend()
    plt.tight_layout()
    plt.savefig(FIGURES / "chronolog_throughput_over_time.png", dpi=180)
    plt.close()


def plot_baseline_ratios(rows: list[dict]) -> None:
    if not rows:
        return
    row = rows[0]
    labels = ["vs iter 0", "vs Kafka", "vs Mofka"]
    values = [
        float(row["chronolog_throughput_ratio_to_iteration0"]),
        float(row["chronolog_to_kafka_throughput_ratio"]),
        float(row["chronolog_to_mofka_throughput_ratio"]),
    ]
    FIGURES.mkdir(parents=True, exist_ok=True)
    plt.figure(figsize=(8.5, 4.8))
    bars = plt.bar(labels, values, color=["#496a9a", "#a05a2c", "#3f7f5f"])
    plt.yscale("log")
    plt.ylim(max(min(values) / 3.0, 1e-6), max(values) * 2.0)
    plt.ylabel("ChronoLog throughput ratio")
    plt.title("Iteration 0 Throughput Ratios to Fixed Baselines")
    plt.grid(axis="y", alpha=0.25, which="both")
    for bar, value in zip(bars, values):
        plt.text(bar.get_x() + bar.get_width() / 2, value, f"{value:.6f}", ha="center", va="bottom", fontsize=9)
    plt.tight_layout()
    plt.savefig(FIGURES / "chronolog_throughput_ratio_to_baselines.png", dpi=180)
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
    plt.figure(figsize=(9.5, 5.4))
    for key, group in sorted(groups.items()):
        group.sort(key=lambda row: int(row["iteration"]))
        x = [int(row["iteration"]) for row in group]
        y = [float(row["total_us"]) / 1000.0 for row in group]
        plt.plot(x, y, marker="o", linewidth=1.8, label=key)
    plt.xlabel("ProfileForge iteration")
    plt.ylabel("observed semantic time (ms)")
    plt.title("TAU Semantic Timing by Optimization Iteration")
    plt.grid(axis="y", alpha=0.25)
    plt.xticks(sorted({int(row["iteration"]) for row in rows}))
    plt.legend(fontsize=8)
    plt.tight_layout()
    plt.savefig(FIGURES / "tau_semantic_time_over_time.png", dpi=180)
    plt.close()


def main() -> None:
    metric_rows = collect_iteration_metrics()
    tau_rows = collect_tau_history()
    write_csv(
        DATA / "chronolog_metrics_history.csv",
        metric_rows,
        [
            "iteration",
            "timestamp",
            "datetime",
            "label",
            "commit",
            "result_dir",
            "profile_mode",
            "nodelist",
            "workflow",
            "node_count",
            "client_count",
            "message_size_bytes",
            "operation_count",
            "duration_seconds",
            "chronolog_throughput_ops_per_sec",
            "chronolog_throughput_ratio_to_iteration0",
            "kafka_baseline_throughput_ops_per_sec",
            "chronolog_to_kafka_throughput_ratio",
            "mofka_baseline_throughput_ops_per_sec",
            "chronolog_to_mofka_throughput_ratio",
            "success",
            "metrics_path",
            "kafka_baseline_metrics_path",
            "mofka_baseline_metrics_path",
            "baseline_note",
        ],
    )
    write_csv(
        DATA / "tau_semantic_history.csv",
        tau_rows,
        ["iteration", "timestamp", "datetime", "label", "result_dir", "role", "region", "count", "total_us", "max_us"],
    )
    plot_throughput_history(metric_rows)
    plot_baseline_ratios(metric_rows)
    plot_tau_history(tau_rows)
    print(f"metric iterations: {len(metric_rows)}")
    print(f"tau semantic rows: {len(tau_rows)}")


if __name__ == "__main__":
    main()
