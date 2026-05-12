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


def role_from_tau_path(path: Path) -> str:
    rel = path.relative_to(ROOT / "raw" / "tau")
    if rel.parts[0] == "client":
        return "client"
    return rel.parts[0]


def read_tau_duration_events() -> list[dict[str, float | str]]:
    rows: list[dict[str, float | str]] = []
    for path in sorted((ROOT / "raw" / "tau").rglob("profile.*")):
        role = role_from_tau_path(path)
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = re.match(r'"([^"]+_duration_us)"\s+(\d+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)', line)
            if not match:
                continue
            name = match.group(1).replace("_duration_us", "")
            count = float(match.group(2))
            max_us = float(match.group(3))
            mean_us = float(match.group(5))
            rows.append({
                "role": role,
                "region": name,
                "count": count,
                "max_us": max_us,
                "mean_us": mean_us,
                "total_us": count * mean_us,
            })
    with (DATA / "tau" / "semantic_region_durations.csv").open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=["role", "region", "count", "mean_us", "max_us", "total_us"])
        writer.writeheader()
        writer.writerows(rows)
    return rows


def read_gperftools_roles() -> list[dict[str, float | str]]:
    rows: list[dict[str, float | str]] = []
    for path in sorted((DATA / "gperftools").glob("*.cpu.pprof.txt")):
        role = path.name.split(".cpu.")[0]
        for name, samples in read_pprof_top(path, limit=6):
            rows.append({"role": role, "symbol": name, "samples": samples})
    with (DATA / "gperftools" / "top_cpu_samples_by_role.csv").open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=["role", "symbol", "samples"])
        writer.writeheader()
        writer.writerows(rows)
    return rows


def read_darshan_summary() -> list[dict[str, float | str]]:
    rows: list[dict[str, float | str]] = []
    wanted = {"POSIX_BYTES_READ", "POSIX_BYTES_WRITTEN", "POSIX_READS", "POSIX_WRITES", "STDIO_BYTES_READ", "STDIO_BYTES_WRITTEN", "STDIO_READS", "STDIO_WRITES"}
    totals: dict[tuple[str, str], float] = {}
    for path in sorted((DATA / "darshan").glob("*.parser.txt")):
        role = path.name.split("-jcernuda_")[0]
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split("\t")
            if len(parts) < 5 or parts[3] not in wanted:
                continue
            try:
                value = float(parts[4])
            except ValueError:
                continue
            if value < 0:
                continue
            totals[(role, parts[3])] = totals.get((role, parts[3]), 0.0) + value
    for (role, metric), value in sorted(totals.items()):
        rows.append({"role": role, "metric": metric, "value": value})
    with (DATA / "darshan" / "io_summary_by_role.csv").open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=["role", "metric", "value"])
        writer.writeheader()
        writer.writerows(rows)
    return rows


def write_profiler_coverage_csv() -> list[tuple[str, str, str]]:
    rows = [
        ("TAU semantic profile", "validated", "distributed client and service profiles"),
        ("gperftools CPU/heap", "validated", "distributed service CPU and heap profiles"),
        ("Darshan", "validated", "distributed client and service logs with non-MPI enabled"),
        ("Linux network measurement commands", "validated", "SLURM node evidence captured"),
        ("perf", "validated", "repo-local kernel-matched perf captured distributed client and service profiles"),
        ("eBPF-based tools", "permission-limited", "unprivileged_bpf_disabled=2; allowlisted wrapper requested"),
        ("TAU trace/Jumpshot timeline", "planned", "semantic duration profile validated; trace-mode timeline is next"),
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


def plot_tau_duration_breakdown(rows: list[dict[str, float | str]]) -> None:
    totals: dict[str, float] = {}
    for row in rows:
        key = f"{row['role']}:{row['region']}"
        totals[key] = totals.get(key, 0.0) + float(row["total_us"]) / 1000.0
    top = sorted(totals.items(), key=lambda item: item[1], reverse=True)[:12]
    names = [name for name, _ in top]
    values = [value for _, value in top]
    plt.figure(figsize=(10, 5.5))
    plt.barh(names, values, color="#2f6f73")
    plt.xlabel("Total semantic time observed (ms)")
    plt.title("ChronoLog TAU Semantic Time Breakdown")
    plt.tight_layout()
    plt.savefig(FIGURES / "tau_semantic_time_breakdown.png", dpi=180)
    plt.close()


def plot_tau_max_latency(rows: list[dict[str, float | str]]) -> None:
    top = sorted(rows, key=lambda row: float(row["max_us"]), reverse=True)[:12]
    names = [f"{row['role']}:{row['region']}" for row in top]
    values = [float(row["max_us"]) / 1000.0 for row in top]
    plt.figure(figsize=(10, 5.5))
    plt.barh(names, values, color="#8a5a44")
    plt.xlabel("Max observed region latency (ms)")
    plt.title("ChronoLog TAU Semantic Tail Latencies")
    plt.tight_layout()
    plt.savefig(FIGURES / "tau_semantic_tail_latency.png", dpi=180)
    plt.close()


def plot_gperftools_by_role(rows: list[dict[str, float | str]]) -> None:
    role_totals: dict[str, float] = {}
    for row in rows:
        role_totals[str(row["role"])] = role_totals.get(str(row["role"]), 0.0) + float(row["samples"])
    roles = list(role_totals)
    values = [role_totals[role] for role in roles]
    plt.figure(figsize=(8, 4.5))
    plt.bar(roles, values, color="#4b6f9f")
    plt.ylabel("Top-symbol CPU samples")
    plt.title("gperftools CPU Sample Signal by Role")
    plt.xticks(rotation=20, ha="right")
    plt.tight_layout()
    plt.savefig(FIGURES / "gperftools_cpu_by_role.png", dpi=180)
    plt.close()


def plot_darshan_io(rows: list[dict[str, float | str]]) -> None:
    totals: dict[str, float] = {}
    for row in rows:
        metric = str(row["metric"])
        if "BYTES" not in metric:
            continue
        totals[str(row["role"])] = totals.get(str(row["role"]), 0.0) + float(row["value"])
    roles = list(totals)
    values = [totals[role] / 1024.0 for role in roles]
    plt.figure(figsize=(8, 4.5))
    plt.bar(roles, values, color="#756bb1")
    plt.ylabel("Darshan observed I/O (KiB)")
    plt.title("Darshan I/O Volume by Role")
    plt.xticks(rotation=20, ha="right")
    plt.tight_layout()
    plt.savefig(FIGURES / "darshan_io_by_role.png", dpi=180)
    plt.close()


def plot_coverage(rows: list[tuple[str, str, str]]) -> None:
    colors = {
        "validated": "#2f6f73",
        "permission-limited": "#b85c38",
        "tool-missing": "#b85c38",
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
    tau_rows = read_tau_duration_events()
    gperf_rows = read_gperftools_roles()
    darshan_rows = read_darshan_summary()
    plot_tau_events(read_tau_events(DATA / "tau-client-pprof.txt"))
    plot_tau_duration_breakdown(tau_rows)
    plot_tau_max_latency(tau_rows)
    plot_pprof(read_pprof_top(DATA / "gperftools-chrono-keeper-pprof.txt"))
    plot_gperftools_by_role(gperf_rows)
    plot_darshan_io(darshan_rows)


if __name__ == "__main__":
    main()
