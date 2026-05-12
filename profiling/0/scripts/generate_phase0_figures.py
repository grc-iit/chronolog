#!/usr/bin/env python3
"""Generate static Phase 0 profiling figures from packaged summaries."""

from __future__ import annotations

import csv
import re
from pathlib import Path

import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data"
FIGURES = ROOT / "figures"


def read_tau_events(path: Path) -> list[tuple[str, float]]:
    events: list[tuple[str, float]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"\s*(\d+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(.+)$", line)
        if not match:
            continue
        name = match.group(2).strip()
        if name in {"Event Name"} or name.startswith("-"):
            continue
        events.append((name, float(match.group(1))))
    return events


def read_pprof_top(path: Path, limit: int = 8) -> list[tuple[str, float]]:
    rows: list[tuple[str, float]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"\s*(\d+)\s+([0-9.]+)%\s+[0-9.]+%\s+\d+\s+[0-9.]+%\s+(.+)$", line)
        if match:
            rows.append((match.group(3).strip(), float(match.group(1))))
    return rows[:limit]


def write_profiler_coverage_csv() -> list[tuple[str, str, str]]:
    rows = [
        ("TAU semantic profile", "validated", "distributed client and service profiles"),
        ("gperftools CPU/heap", "validated", "distributed service CPU and heap profiles"),
        ("Darshan", "validated", "distributed client and service logs with non-MPI enabled"),
        ("Linux network measurement commands", "validated", "SLURM node evidence captured"),
        ("perf", "permission-limited", "kernel perf_event policy requires admin change"),
        ("eBPF-based tools", "permission-limited", "kernel tracing permissions require admin change"),
        ("TAU trace/Jumpshot timeline", "planned", "TAU profile validated; trace-mode timeline is next"),
    ]
    with (DATA / "profiler_coverage.csv").open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(["collector", "status", "evidence"])
        writer.writerows(rows)
    return rows


def plot_tau_events(events: list[tuple[str, float]]) -> None:
    names = [name for name, _ in events]
    values = [value for _, value in events]
    plt.figure(figsize=(8, 4.5))
    plt.barh(names, values, color="#2f6f73")
    plt.xlabel("TAU user-event samples")
    plt.title("ChronoLog TAU Semantic Events")
    plt.tight_layout()
    plt.savefig(FIGURES / "tau_semantic_events.png", dpi=180)
    plt.close()


def plot_pprof(rows: list[tuple[str, float]]) -> None:
    names = [name[:42] for name, _ in rows]
    values = [value for _, value in rows]
    plt.figure(figsize=(9, 4.8))
    plt.barh(names, values, color="#8a5a44")
    plt.xlabel("CPU samples")
    plt.title("gperftools ChronoKeeper CPU Sample Distribution")
    plt.tight_layout()
    plt.savefig(FIGURES / "gperftools_keeper_cpu_samples.png", dpi=180)
    plt.close()


def plot_coverage(rows: list[tuple[str, str, str]]) -> None:
    colors = {
        "validated": "#2f6f73",
        "permission-limited": "#b85c38",
        "planned": "#6b6f7a",
    }
    names = [row[0] for row in rows]
    y = list(range(len(rows)))
    plt.figure(figsize=(9, 4.8))
    plt.barh(y, [1] * len(rows), color=[colors[row[1]] for row in rows])
    plt.yticks(y, names)
    plt.xticks([])
    plt.title("Phase 0 Profiler Coverage")
    for i, (_, status, _) in enumerate(rows):
        plt.text(0.03, i, status, va="center", ha="left", color="white", fontweight="bold")
    plt.tight_layout()
    plt.savefig(FIGURES / "profiler_coverage.png", dpi=180)
    plt.close()


def main() -> None:
    FIGURES.mkdir(parents=True, exist_ok=True)
    DATA.mkdir(parents=True, exist_ok=True)
    coverage = write_profiler_coverage_csv()
    plot_coverage(coverage)
    plot_tau_events(read_tau_events(DATA / "tau-client-pprof.txt"))
    plot_pprof(read_pprof_top(DATA / "gperftools-chrono-keeper-pprof.txt"))


if __name__ == "__main__":
    main()
