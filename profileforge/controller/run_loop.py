#!/usr/bin/env python3
"""Run the ProfileForge benchmark/profile/validate/judge loop."""

from __future__ import annotations

import argparse
import csv
import json
import os
import shlex
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
RESULTS_ROOT = REPO_ROOT / "profileforge" / "results"
MATRIX = REPO_ROOT / ".agent" / "scripts" / "phase0_benchmark_matrix.py"
VALIDATOR = REPO_ROOT / "profileforge" / "validators" / "validate_correctness.py"
NORMALIZER = REPO_ROOT / "profileforge" / "controller" / "normalize_evidence.py"
JUDGE = REPO_ROOT / "profileforge" / "controller" / "performance_judge.py"


def timestamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def run_command(cmd: list[str], cwd: Path, log_path: Path, dry_run: bool = False) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    rendered = " ".join(shlex.quote(part) for part in cmd)
    with log_path.open("a", encoding="utf-8") as log:
        log.write(f"$ {rendered}\n")
        log.flush()
        if dry_run:
            return 0
        proc = subprocess.run(cmd, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, text=True)
        log.write(f"exit_code={proc.returncode}\n")
        return proc.returncode


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def read_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--systems", default="chronolog")
    parser.add_argument("--workflows", default="append_throughput")
    parser.add_argument("--node-counts", default="2")
    parser.add_argument("--message-sizes", default="1024")
    parser.add_argument("--operation-counts", default="100")
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--partition", default=os.environ.get("CHRONOLOG_SLURM_PARTITION", "debug"))
    parser.add_argument("--nodelist", default=os.environ.get("CHRONOLOG_NODELIST", ""))
    parser.add_argument("--slurm-time", default="00:10:00")
    parser.add_argument("--chronolog-profile-mode", default="tau")
    parser.add_argument("--chronolog-install-dir", default=str(REPO_ROOT / ".agent" / "install-tau" / "chronolog"))
    parser.add_argument("--chronolog-startup-sleep", type=int, default=10)
    parser.add_argument("--perf-bin", default=str(REPO_ROOT / "opt" / "perf" / "bin" / "perf"))
    parser.add_argument("--patch-command", default="")
    parser.add_argument("--result-root", default="")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def matrix_command(args: argparse.Namespace, result_dir: Path) -> list[str]:
    cmd = [
        "python3",
        str(MATRIX),
        "--systems",
        args.systems,
        "--workflows",
        args.workflows,
        "--node-counts",
        args.node_counts,
        "--message-sizes",
        args.message_sizes,
        "--operation-counts",
        args.operation_counts,
        "--trials",
        str(args.trials),
        "--partition",
        args.partition,
        "--slurm-time",
        args.slurm_time,
        "--chronolog-profile-mode",
        args.chronolog_profile_mode,
        "--chronolog-install-dir",
        args.chronolog_install_dir,
        "--chronolog-startup-sleep",
        str(args.chronolog_startup_sleep),
        "--perf-bin",
        args.perf_bin,
        "--result-dir",
        str(result_dir),
    ]
    if args.nodelist:
        cmd.extend(["--nodelist", args.nodelist])
    if args.dry_run:
        cmd.append("--dry-run")
    return cmd


def validate_rows(iteration_dir: Path, rows: list[dict[str, str]], dry_run: bool) -> list[str]:
    correctness_paths: list[str] = []
    for index, row in enumerate(rows, start=1):
        if row.get("system") != "chronolog":
            continue
        if not row.get("metrics_path"):
            continue
        metrics_path = Path(row.get("metrics_path", ""))
        result_dir = Path(row.get("result_dir", ""))
        if not metrics_path.is_absolute():
            metrics_path = REPO_ROOT / metrics_path
        if not result_dir.is_absolute():
            result_dir = REPO_ROOT / result_dir
        output = result_dir / "correctness.json"
        cmd = [
            "python3",
            str(VALIDATOR),
            "--metrics",
            str(metrics_path),
            "--result-dir",
            str(result_dir),
            "--output",
            str(output),
        ]
        code = run_command(cmd, REPO_ROOT, iteration_dir / "controller.log", dry_run=dry_run)
        if code == 0 or output.exists():
            correctness_paths.append(str(output))
        elif dry_run:
            correctness_paths.append(str(output))
        else:
            raise RuntimeError(f"correctness validation failed for row {index}: {metrics_path}")
    return correctness_paths


def normalize_rows(iteration_dir: Path, rows: list[dict[str, str]], dry_run: bool) -> list[str]:
    evidence_paths: list[str] = []
    for index, row in enumerate(rows, start=1):
        if row.get("system") != "chronolog":
            continue
        if not row.get("result_dir"):
            continue
        result_dir = Path(row.get("result_dir", ""))
        if not result_dir.is_absolute():
            result_dir = REPO_ROOT / result_dir
        output = result_dir / "evidence.json"
        cmd = [
            "python3",
            str(NORMALIZER),
            "--result-dir",
            str(result_dir),
            "--label",
            f"iteration_candidate_{index}",
            "--output",
            str(output),
        ]
        code = run_command(cmd, REPO_ROOT, iteration_dir / "controller.log", dry_run=dry_run)
        if code == 0 or output.exists() or dry_run:
            evidence_paths.append(str(output))
        else:
            raise RuntimeError(f"evidence normalization failed for row {index}: {result_dir}")
    return evidence_paths


def judge_iteration(iteration_dir: Path, rows: list[dict[str, str]], correctness_paths: list[str], dry_run: bool) -> Path:
    metrics_paths = [row["metrics_path"] for row in rows if row.get("system") == "chronolog" and row.get("metrics_path")]
    output = iteration_dir / "judge.json"
    cmd = ["python3", str(JUDGE), "--metrics", *metrics_paths, "--correctness", *correctness_paths, "--output", str(output)]
    code = run_command(cmd, REPO_ROOT, iteration_dir / "controller.log", dry_run=dry_run)
    if dry_run:
        output.write_text(
            json.dumps({"judge": "profileforge_performance_judge", "decision": "dry_run", "command": cmd}, indent=2) + "\n",
            encoding="utf-8",
        )
    elif code not in {0, 1}:
        raise RuntimeError("performance judge crashed")
    return output


def main() -> int:
    args = parse_args()
    root = Path(args.result_root).resolve() if args.result_root else RESULTS_ROOT / f"loop-{timestamp()}"
    root.mkdir(parents=True, exist_ok=True)

    loop_summary: list[dict[str, Any]] = []
    for iteration in range(1, args.iterations + 1):
        iteration_dir = root / f"iteration-{iteration:03d}"
        iteration_dir.mkdir(parents=True, exist_ok=True)
        if args.patch_command:
            code = run_command(["bash", "-lc", args.patch_command], REPO_ROOT, iteration_dir / "controller.log", dry_run=args.dry_run)
            if code != 0:
                raise RuntimeError(f"patch command failed for iteration {iteration}")

        matrix_dir = iteration_dir / "matrix"
        code = run_command(matrix_command(args, matrix_dir), REPO_ROOT, iteration_dir / "controller.log", dry_run=False)
        if code != 0 and not args.dry_run:
            raise RuntimeError(f"benchmark matrix failed for iteration {iteration}")

        summary_csv = matrix_dir / "benchmark-matrix" / "summary.csv"
        rows = read_csv(summary_csv) if summary_csv.exists() else []
        correctness_paths = validate_rows(iteration_dir, rows, dry_run=args.dry_run) if rows else []
        evidence_paths = normalize_rows(iteration_dir, rows, dry_run=args.dry_run) if rows else []
        judge_path = judge_iteration(iteration_dir, rows, correctness_paths, dry_run=args.dry_run) if rows else iteration_dir / "judge.json"
        judge = read_json(judge_path) if judge_path.exists() else {"decision": "missing"}

        summary = {
            "iteration": iteration,
            "iteration_dir": display_path(iteration_dir),
            "matrix_summary": display_path(summary_csv) if summary_csv.exists() else "",
            "correctness": [display_path(Path(path)) for path in correctness_paths if Path(path).exists()],
            "evidence": [display_path(Path(path)) for path in evidence_paths if Path(path).exists()],
            "judge": display_path(judge_path) if judge_path.exists() else "",
            "decision": judge.get("decision"),
        }
        loop_summary.append(summary)
        (iteration_dir / "iteration-summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        (root / "loop-summary.json").write_text(json.dumps(loop_summary, indent=2) + "\n", encoding="utf-8")

    print(display_path(root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
