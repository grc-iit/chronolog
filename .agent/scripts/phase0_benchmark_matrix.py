#!/usr/bin/env python3
"""Run a tunable Phase 0 benchmark matrix and collect common metrics."""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
RESULTS_ROOT = REPO_ROOT / ".agent" / "results"


SYSTEM_SCRIPTS = {
    "chronolog": REPO_ROOT / ".agent" / "scripts" / "chronolog_run_append_distributed.sh",
    "kafka": REPO_ROOT / ".agent" / "scripts" / "kafka_run_append_distributed.sh",
    "mofka": REPO_ROOT / ".agent" / "scripts" / "mofka_run_append_smoke.sh",
}


WORKFLOW_BY_SYSTEM = {
    "append_throughput": {"chronolog", "kafka", "mofka"},
    "append_latency": {"chronolog"},
    "range_retrieval": {"chronolog", "kafka"},
}


def timestamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def csv_ints(value: str) -> list[int]:
    return [int(part) for part in value.split(",") if part.strip()]


def csv_strings(value: str) -> list[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--systems", default="chronolog,kafka,mofka")
    parser.add_argument("--workflows", default="append_throughput")
    parser.add_argument("--node-counts", default="2")
    parser.add_argument("--message-sizes", default="1024")
    parser.add_argument("--operation-counts", default="100")
    parser.add_argument("--trials", type=int, default=1)
    parser.add_argument("--partition", default=os.environ.get("CHRONOLOG_SLURM_PARTITION", "debug"))
    parser.add_argument("--nodelist", default=os.environ.get("CHRONOLOG_NODELIST", ""))
    parser.add_argument("--slurm-time", default="00:10:00")
    parser.add_argument("--chronolog-install-dir", default=str(REPO_ROOT / ".agent" / "install-tau" / "chronolog"))
    parser.add_argument("--chronolog-profile-mode", default="none")
    parser.add_argument("--chronolog-startup-sleep", type=int, default=10)
    parser.add_argument("--perf-bin", default=str(REPO_ROOT / "opt" / "perf" / "bin" / "perf"))
    parser.add_argument("--result-dir", default="")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def run_dir(args: argparse.Namespace) -> Path:
    if args.result_dir:
        root = Path(args.result_dir).resolve()
    else:
        root = RESULTS_ROOT / timestamp()
    root.mkdir(parents=True, exist_ok=True)
    return root


def command_for(run: dict[str, Any], child_dir: Path, args: argparse.Namespace) -> list[str]:
    system = run["system"]
    workflow = run["workflow"]
    script = SYSTEM_SCRIPTS[system]
    base = [
        str(script),
        "--result-dir",
        str(child_dir),
        "--partition",
        args.partition,
        "--node-count",
        str(run["node_count"]),
        "--operation-count",
        str(run["operation_count"]),
        "--message-size-bytes",
        str(run["message_size_bytes"]),
    ]
    if system in {"chronolog", "kafka"}:
        base.extend(["--workflow", workflow, "--slurm-time", args.slurm_time])
    if system == "chronolog":
        base.extend(["--install-dir", args.chronolog_install_dir, "--profile-mode", args.chronolog_profile_mode])
        if args.nodelist:
            base.extend(["--nodelist", args.nodelist])
        if args.chronolog_profile_mode == "perf":
            base.extend(["--perf-bin", args.perf_bin])
    return base


def metrics_path(system: str, child_dir: Path) -> Path:
    return child_dir / system / "metrics.json"


def flatten_metrics(path: Path, run: dict[str, Any]) -> dict[str, Any]:
    row = dict(run)
    row["result_dir"] = str(path.parents[1])
    row["metrics_path"] = str(path)
    if not path.exists():
        row["success"] = False
        row["error"] = "metrics.json missing"
        return row
    data = json.loads(path.read_text(encoding="utf-8"))
    for key, value in data.items():
        row[key] = value
    row["error"] = ""
    return row


def write_summary(rows: list[dict[str, Any]], path: Path) -> None:
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
        "error",
        "metrics_path",
        "result_dir",
    ]
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    systems = csv_strings(args.systems)
    workflows = csv_strings(args.workflows)
    node_counts = csv_ints(args.node_counts)
    message_sizes = csv_ints(args.message_sizes)
    operation_counts = csv_ints(args.operation_counts)
    root = run_dir(args)
    matrix_dir = root / "benchmark-matrix"
    matrix_dir.mkdir(parents=True, exist_ok=True)

    runs: list[dict[str, Any]] = []
    for system, workflow, node_count, message_size, op_count, trial in itertools.product(
        systems, workflows, node_counts, message_sizes, operation_counts, range(1, args.trials + 1)
    ):
        if workflow not in WORKFLOW_BY_SYSTEM or system not in WORKFLOW_BY_SYSTEM[workflow]:
            continue
        runs.append(
            {
                "system": system,
                "workflow": workflow,
                "trial": trial,
                "node_count": node_count,
                "message_size_bytes": message_size,
                "operation_count": op_count,
            }
        )

    (matrix_dir / "matrix-expanded.json").write_text(json.dumps(runs, indent=2) + "\n", encoding="utf-8")
    rows: list[dict[str, Any]] = []
    command_lines: list[str] = []
    for index, run in enumerate(runs, start=1):
        child_dir = root / f"{index:03d}-{run['system']}-{run['workflow']}-n{run['node_count']}-s{run['message_size_bytes']}-t{run['trial']}"
        cmd = command_for(run, child_dir, args)
        command_lines.append(" ".join(subprocess.list2cmdline([part]) for part in cmd))
        if args.dry_run:
            row = dict(run)
            row.update({"success": None, "error": "dry-run", "result_dir": str(child_dir), "metrics_path": ""})
            rows.append(row)
            continue
        child_dir.mkdir(parents=True, exist_ok=True)
        env = os.environ.copy()
        env["PHASE0_RESULT_DIR"] = str(child_dir)
        if run["system"] == "chronolog":
            env["CHRONOLOG_STARTUP_SLEEP_SECONDS"] = str(args.chronolog_startup_sleep)
        proc = subprocess.run(cmd, cwd=REPO_ROOT, env=env, text=True)
        row = flatten_metrics(metrics_path(run["system"], child_dir), run)
        if proc.returncode != 0:
            row["success"] = False
            row["error"] = f"command exited {proc.returncode}"
        rows.append(row)
        write_summary(rows, matrix_dir / "summary.csv")
        (matrix_dir / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")

    (matrix_dir / "commands.sh").write_text("\n".join(command_lines) + "\n", encoding="utf-8")
    write_summary(rows, matrix_dir / "summary.csv")
    (matrix_dir / "summary.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    (root / "summary.md").write_text(
        "# Phase 0 Benchmark Matrix\n\n"
        f"- runs: {len(runs)}\n"
        f"- dry_run: {args.dry_run}\n"
        f"- summary: benchmark-matrix/summary.csv\n",
        encoding="utf-8",
    )
    print(matrix_dir)
    return 0 if all(row.get("success") in {True, None} for row in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
