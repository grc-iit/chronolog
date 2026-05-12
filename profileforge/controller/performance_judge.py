#!/usr/bin/env python3
"""Judge repeated ProfileForge benchmark results against prior and fixed baselines."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
HISTORY = REPO_ROOT / "profiling" / "data" / "history"
DEFAULT_POLICY = REPO_ROOT / "profileforge" / "controller" / "acceptance-policy.yaml"


def repo_path(text: str) -> Path:
    path = Path(text)
    if path.is_absolute():
        return path
    return REPO_ROOT / path


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def read_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


def load_metrics(paths: list[str]) -> list[dict[str, Any]]:
    rows = []
    for item in paths:
        path = repo_path(item)
        data = read_json(path)
        data["_metrics_path"] = display_path(path)
        rows.append(data)
    return rows


def latest_iteration0_baseline(workflow: str, node_count: int, message_size: int) -> float | None:
    rows = read_csv(HISTORY / "chronolog_metrics_history.csv")
    candidates = [
        row for row in rows
        if row["workflow"] == workflow
        and int(row["node_count"]) == node_count
        and int(row["message_size_bytes"]) == message_size
    ]
    if not candidates:
        return None
    candidates.sort(key=lambda row: int(row["iteration"]))
    return float(candidates[0]["chronolog_throughput_ops_per_sec"])


def fixed_baselines(workflow: str, node_count: int, message_size: int) -> dict[str, float]:
    out: dict[str, float] = {}
    for row in read_csv(HISTORY / "fixed_baselines.csv"):
        if (
            row["workflow"] == workflow
            and int(row["node_count"]) == node_count
            and int(row["message_size_bytes"]) == message_size
        ):
            metrics = read_json(repo_path(row["metrics_path"]))
            out[row["system"]] = float(metrics["throughput_ops_per_sec"])
    return out


def judge(metrics: list[dict[str, Any]], correctness: list[dict[str, Any]]) -> dict[str, Any]:
    successful = [row for row in metrics if row.get("success") is True]
    throughputs = [float(row["throughput_ops_per_sec"]) for row in successful]
    workflow = str(metrics[0].get("workflow", "")) if metrics else ""
    node_count = int(metrics[0].get("node_count", 0)) if metrics else 0
    message_size = int(metrics[0].get("message_size_bytes", 0)) if metrics else 0
    base = latest_iteration0_baseline(workflow, node_count, message_size)
    fixed = fixed_baselines(workflow, node_count, message_size)

    mean = statistics.fmean(throughputs) if throughputs else 0.0
    stdev = statistics.stdev(throughputs) if len(throughputs) > 1 else 0.0
    rel_stdev = stdev / mean if mean > 0 else None
    correctness_pass = all(row.get("result") == "pass" for row in correctness) and len(correctness) == len(metrics)
    improvement = ((mean / base) - 1.0) * 100.0 if base else None

    reasons: list[str] = []
    decision = "reject"
    if not metrics:
        reasons.append("no metrics supplied")
    if len(successful) != len(metrics):
        reasons.append("one or more benchmark trials failed")
    if not correctness_pass:
        reasons.append("one or more correctness validators failed")
    if len(metrics) < 3:
        reasons.append("fewer than 3 repeated runs; cannot accept performance change")
    if rel_stdev is not None and rel_stdev > 0.15:
        reasons.append(f"relative throughput stdev {rel_stdev:.3f} exceeds 0.15")
    if base is not None and improvement is not None and improvement < 5.0:
        reasons.append(f"throughput improvement {improvement:.2f}% below 5% acceptance threshold")

    if not reasons:
        decision = "accept"
    elif metrics and len(successful) == len(metrics) and correctness_pass:
        decision = "rerun_or_investigate"

    return {
        "judge": "profileforge_performance_judge",
        "decision": decision,
        "reasons": reasons,
        "trial_count": len(metrics),
        "successful_trial_count": len(successful),
        "workflow": workflow,
        "node_count": node_count,
        "message_size_bytes": message_size,
        "throughput_ops_per_sec": {
            "mean": mean,
            "stdev": stdev,
            "relative_stdev": rel_stdev,
            "values": throughputs,
        },
        "chronolog_iteration0_throughput_ops_per_sec": base,
        "throughput_improvement_percent_vs_iteration0": improvement,
        "fixed_baselines": fixed,
        "ratios": {
            f"chronolog_to_{system}": (mean / value if value else None)
            for system, value in fixed.items()
        },
        "correctness_results": correctness,
        "metrics": metrics,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metrics", nargs="+", required=True)
    parser.add_argument("--correctness", nargs="*", default=[])
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    metrics = load_metrics(args.metrics)
    correctness = [read_json(repo_path(path)) for path in args.correctness]
    report = judge(metrics, correctness)
    output = repo_path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(display_path(output))
    return 0 if report["decision"] == "accept" else 1


if __name__ == "__main__":
    raise SystemExit(main())
