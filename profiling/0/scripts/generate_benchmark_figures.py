#!/usr/bin/env python3
"""Generate Phase 0 benchmark matrix plots from collected metrics."""

from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
DATA = ROOT / "data" / "benchmark"
FIGURES = ROOT / "figures"


def read_rows() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for path in sorted((REPO_ROOT / ".agent" / "results").glob("*/benchmark-matrix/summary.csv")):
        with path.open(encoding="utf-8") as fh:
            summary_rows = list(csv.DictReader(fh))
        if not summary_rows or any(row.get("success") != "True" for row in summary_rows):
            continue
        for row in summary_rows:
            row["summary_path"] = str(path)
            rows.append(row)
    return rows


def write_combined(rows: list[dict[str, str]]) -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    keys = [
        "system",
        "workflow",
        "trial",
        "node_count",
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
        "result_dir",
        "summary_path",
    ]
    with (DATA / "benchmark_matrix_summary.csv").open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def plot_throughput(rows: list[dict[str, str]]) -> None:
    points: dict[tuple[str, str, int], list[tuple[int, float]]] = {}
    for row in rows:
        if row.get("workflow") != "append_throughput":
            continue
        key = (row["system"], row["workflow"], int(float(row["node_count"])))
        points.setdefault(key, []).append((int(float(row["message_size_bytes"])), float(row["throughput_ops_per_sec"])))
    if not points:
        return
    FIGURES.mkdir(parents=True, exist_ok=True)
    plt.figure(figsize=(8, 4.5))
    for key, values in sorted(points.items()):
        values.sort()
        label = f"{key[0]} n={key[2]}"
        plt.plot([v[0] for v in values], [v[1] for v in values], marker="o", label=label)
    plt.xlabel("message size (bytes)")
    plt.ylabel("throughput (ops/s)")
    plt.title("Phase 0 append throughput matrix")
    plt.grid(axis="y", alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(FIGURES / "benchmark_append_throughput_matrix.png", dpi=180)
    plt.close()


def plot_latency(rows: list[dict[str, str]]) -> None:
    points: dict[tuple[str, str, int], list[tuple[int, float]]] = {}
    for row in rows:
        latency = row.get("p95_latency_ms") or row.get("avg_latency_ms")
        if not latency or latency == "None":
            continue
        key = (row["system"], row["workflow"], int(float(row["node_count"])))
        points.setdefault(key, []).append((int(float(row["message_size_bytes"])), float(latency)))
    if not points:
        return
    FIGURES.mkdir(parents=True, exist_ok=True)
    plt.figure(figsize=(8, 4.5))
    for key, values in sorted(points.items()):
        values.sort()
        label = f"{key[0]} {key[1]} n={key[2]}"
        plt.plot([v[0] for v in values], [v[1] for v in values], marker="o", label=label)
    plt.xlabel("message size (bytes)")
    plt.ylabel("latency (ms)")
    plt.title("Phase 0 latency matrix")
    plt.grid(axis="y", alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(FIGURES / "benchmark_latency_matrix.png", dpi=180)
    plt.close()


def main() -> None:
    rows = read_rows()
    write_combined(rows)
    plot_throughput(rows)
    plot_latency(rows)
    print(f"benchmark rows: {len(rows)}")


if __name__ == "__main__":
    main()
