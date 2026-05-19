#!/usr/bin/env python3
"""Judge repeated ProfileForge benchmark results against fixed baselines."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
HISTORY = REPO_ROOT / "profiling" / "data" / "history"
LOWER_IS_BETTER = {"append_latency"}


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


def objective_metric(workflow: str, row: dict[str, Any]) -> tuple[str, float | None, bool]:
    if workflow in LOWER_IS_BETTER:
        for key in ["p99_latency_ms", "p95_latency_ms", "avg_latency_ms"]:
            value = row.get(key)
            if isinstance(value, (int, float)) and value > 0:
                return key, float(value), True
        return "p99_latency_ms", None, True
    value = row.get("throughput_ops_per_sec")
    return "throughput_ops_per_sec", float(value) if isinstance(value, (int, float)) else None, False


def baseline_metric(system: str, workflow: str, node_count: int, message_size: int) -> dict[str, Any] | None:
    for row in read_csv(HISTORY / "fixed_baselines.csv"):
        if (
            row["system"] == system
            and row["workflow"] == workflow
            and int(row["node_count"]) == node_count
            and int(row["message_size_bytes"]) == message_size
        ):
            metrics = read_json(repo_path(row["metrics_path"]))
            key, value, lower_is_better = objective_metric(workflow, metrics)
            return {
                "system": system,
                "metrics_path": row["metrics_path"],
                "metric": key,
                "value": value,
                "lower_is_better": lower_is_better,
            }
    return None


def iteration0_metric(workflow: str, node_count: int, message_size: int) -> dict[str, Any] | None:
    for row in read_csv(HISTORY / "chronolog_metrics_history.csv"):
        if (
            row["workflow"] == workflow
            and int(row["node_count"]) == node_count
            and int(row["message_size_bytes"]) == message_size
        ):
            metrics = read_json(repo_path(row["metrics_path"]))
            key, value, lower_is_better = objective_metric(workflow, metrics)
            return {
                "metrics_path": row["metrics_path"],
                "metric": key,
                "value": value,
                "lower_is_better": lower_is_better,
            }
    return None


def ratio(current: float, baseline: float, lower_is_better: bool) -> float | None:
    if current <= 0 or baseline <= 0:
        return None
    return baseline / current if lower_is_better else current / baseline


def group_key(row: dict[str, Any]) -> tuple[str, int, int]:
    return (
        str(row.get("workflow", "")),
        int(row.get("node_count", 0) or 0),
        int(row.get("message_size_bytes", 0) or 0),
    )


def judge_group(
    key: tuple[str, int, int],
    metrics: list[dict[str, Any]],
    correctness: list[dict[str, Any]],
    goal_system: str,
    goal_ratio: float,
    minimum_trials: int,
) -> dict[str, Any]:
    workflow, node_count, message_size = key
    successful = [row for row in metrics if row.get("success") is True]
    metric_name = objective_metric(workflow, metrics[0])[0] if metrics else "throughput_ops_per_sec"
    lower_is_better = objective_metric(workflow, metrics[0])[2] if metrics else False
    values = [objective_metric(workflow, row)[1] for row in successful]
    numeric_values = [float(value) for value in values if value is not None]

    mean = statistics.fmean(numeric_values) if numeric_values else 0.0
    stdev = statistics.stdev(numeric_values) if len(numeric_values) > 1 else 0.0
    rel_stdev = stdev / mean if mean > 0 else None
    correctness_pass = all(row.get("result") == "pass" for row in correctness) and len(correctness) == len(metrics)

    iter0 = iteration0_metric(workflow, node_count, message_size)
    fixed = {
        system: baseline_metric(system, workflow, node_count, message_size)
        for system in ["kafka", "mofka"]
    }
    fixed = {system: data for system, data in fixed.items() if data is not None}

    ratios = {}
    if iter0 and iter0["value"]:
        ratios["chronolog_to_iteration0"] = ratio(mean, float(iter0["value"]), lower_is_better)
    for system, data in fixed.items():
        if data["value"]:
            ratios[f"chronolog_to_{system}"] = ratio(mean, float(data["value"]), lower_is_better)

    reasons: list[str] = []
    if len(successful) != len(metrics):
        reasons.append("one or more benchmark trials failed")
    if not correctness_pass:
        reasons.append("one or more correctness validators failed")
    if len(metrics) < minimum_trials:
        reasons.append(f"fewer than {minimum_trials} repeated runs")
    if rel_stdev is not None and rel_stdev > 0.15:
        reasons.append(f"relative {metric_name} stdev {rel_stdev:.3f} exceeds 0.15")
    goal_key = f"chronolog_to_{goal_system}"
    goal_value = ratios.get(goal_key)
    if goal_value is None:
        reasons.append(f"missing {goal_system} fixed baseline for {workflow}, node_count={node_count}, message_size={message_size}")
    elif goal_value < goal_ratio:
        reasons.append(f"{goal_key} ratio {goal_value:.6f} below goal {goal_ratio:.6f}")

    decision = "goal_met" if not reasons else "continue"
    if metrics and len(successful) != len(metrics) or not correctness_pass:
        decision = "reject"

    return {
        "workflow": workflow,
        "node_count": node_count,
        "message_size_bytes": message_size,
        "metric": metric_name,
        "lower_is_better": lower_is_better,
        "decision": decision,
        "reasons": reasons,
        "trial_count": len(metrics),
        "successful_trial_count": len(successful),
        "mean": mean,
        "stdev": stdev,
        "relative_stdev": rel_stdev,
        "values": numeric_values,
        "iteration0_baseline": iter0,
        "fixed_baselines": fixed,
        "ratios": ratios,
        "correctness_results": correctness,
        "metrics": metrics,
    }


def judge(metrics: list[dict[str, Any]], correctness: list[dict[str, Any]], goal_system: str, goal_ratio: float, minimum_trials: int) -> dict[str, Any]:
    metrics_by_group: dict[tuple[str, int, int], list[dict[str, Any]]] = {}
    for row in metrics:
        metrics_by_group.setdefault(group_key(row), []).append(row)
    correctness_by_group: dict[tuple[str, int, int], list[dict[str, Any]]] = {}
    for row in correctness:
        correctness_by_group.setdefault(
            (str(row.get("workflow", "")), int(row.get("node_count", 0) or 0), int(row.get("message_size_bytes", 0) or 0)),
            [],
        ).append(row)

    groups = [
        judge_group(key, rows, correctness_by_group.get(key, []), goal_system, goal_ratio, minimum_trials)
        for key, rows in sorted(metrics_by_group.items())
    ]
    goal_met = bool(groups) and all(group["decision"] == "goal_met" for group in groups)
    has_reject = any(group["decision"] == "reject" for group in groups)
    decision = "goal_met" if goal_met else "reject" if has_reject else "continue"
    return {
        "judge": "profileforge_performance_judge",
        "decision": decision,
        "goal": {
            "system": goal_system,
            "ratio": goal_ratio,
            "meaning": "ChronoLog objective metric must be at least this multiple better than the fixed baseline for every judged group.",
        },
        "minimum_trials": minimum_trials,
        "groups": groups,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metrics", nargs="+", required=True)
    parser.add_argument("--correctness", nargs="*", default=[])
    parser.add_argument("--output", required=True)
    parser.add_argument("--goal-system", default="mofka")
    parser.add_argument("--goal-ratio", type=float, default=2.0)
    parser.add_argument("--minimum-trials", type=int, default=3)
    args = parser.parse_args()

    metrics = load_metrics(args.metrics)
    correctness = [read_json(repo_path(path)) for path in args.correctness]
    report = judge(metrics, correctness, args.goal_system, args.goal_ratio, args.minimum_trials)
    output = repo_path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(display_path(output))
    return 0 if report["decision"] == "goal_met" else 1


if __name__ == "__main__":
    raise SystemExit(main())
