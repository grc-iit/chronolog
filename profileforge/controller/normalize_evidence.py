#!/usr/bin/env python3
"""Normalize ProfileForge iteration evidence into one agent-facing JSON file."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
PROFILING = REPO_ROOT / "profiling"
HISTORY = PROFILING / "data" / "history"
PROFILEFORGE_RESULTS = REPO_ROOT / "profileforge" / "results"


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def read_json(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


def repo_path(text: str) -> Path:
    path = Path(text)
    if path.is_absolute():
        return path
    return REPO_ROOT / path


def iteration_row(iteration: int) -> dict[str, str]:
    for row in read_csv(HISTORY / "iteration_map.csv"):
        if int(row["iteration"]) == iteration:
            return row
    raise SystemExit(f"iteration {iteration} not found in {HISTORY / 'iteration_map.csv'}")


def metrics_row(iteration: int) -> dict[str, str]:
    for row in read_csv(HISTORY / "chronolog_metrics_history.csv"):
        if int(row["iteration"]) == iteration:
            return row
    raise SystemExit(f"iteration {iteration} not found in {HISTORY / 'chronolog_metrics_history.csv'}")


def tau_summary(iteration: int, limit: int = 12) -> dict[str, Any]:
    rows = [row for row in read_csv(HISTORY / "tau_semantic_history.csv") if int(row["iteration"]) == iteration]
    rows.sort(key=lambda row: float(row["total_us"]), reverse=True)
    return {
        "source": str((HISTORY / "tau_semantic_history.csv").relative_to(REPO_ROOT)),
        "top_regions": [
            {
                "role": row["role"],
                "region": row["region"],
                "count": float(row["count"]),
                "total_us": float(row["total_us"]),
                "max_us": float(row["max_us"]),
            }
            for row in rows[:limit]
        ],
    }


def csv_summary(path: Path, limit: int = 12) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    rows = read_csv(path)
    return rows[:limit]


def text_head(path: Path, lines: int = 40) -> list[str]:
    if not path.exists():
        return []
    return path.read_text(encoding="utf-8", errors="replace").splitlines()[:lines]


def artifact(path: Path) -> str:
    return str(path.relative_to(REPO_ROOT)) if path.exists() else ""


def build_evidence(iteration: int) -> dict[str, Any]:
    item = iteration_row(iteration)
    metric = metrics_row(iteration)
    result_dir = repo_path(item["result_dir"])
    metrics_path = repo_path(metric["metrics_path"])
    raw_metrics = read_json(metrics_path)

    phase0 = PROFILING / "0"
    evidence = {
        "iteration": iteration,
        "timestamp": item["timestamp"],
        "label": item.get("label", ""),
        "commit": item.get("commit", ""),
        "target": "chronolog",
        "result_dir": item["result_dir"],
        "benchmark": {
            "workflow": raw_metrics.get("workflow"),
            "node_count": raw_metrics.get("node_count"),
            "client_count": raw_metrics.get("client_count"),
            "message_size_bytes": raw_metrics.get("message_size_bytes"),
            "operation_count": raw_metrics.get("operation_count"),
            "duration_seconds": raw_metrics.get("duration_seconds"),
            "throughput_ops_per_sec": raw_metrics.get("throughput_ops_per_sec"),
            "record_event_bandwidth_mb_per_sec": raw_metrics.get("record_event_bandwidth_mb_per_sec"),
            "success": raw_metrics.get("success"),
            "metrics_path": metric["metrics_path"],
        },
        "baseline_ratios": {
            "chronolog_to_iteration0": float(metric["chronolog_throughput_ratio_to_iteration0"]),
            "chronolog_to_kafka": float(metric["chronolog_to_kafka_throughput_ratio"]),
            "chronolog_to_mofka": float(metric["chronolog_to_mofka_throughput_ratio"]),
            "kafka_baseline_metrics_path": metric["kafka_baseline_metrics_path"],
            "mofka_baseline_metrics_path": metric["mofka_baseline_metrics_path"],
            "note": metric["baseline_note"],
        },
        "profilers": {
            "tau": tau_summary(iteration),
            "gperftools": {
                "top_cpu_samples_by_role": csv_summary(phase0 / "data" / "gperftools" / "top_cpu_samples_by_role.csv"),
                "artifact_dir": artifact(phase0 / "data" / "gperftools"),
            },
            "darshan": {
                "io_summary_by_role": csv_summary(phase0 / "data" / "darshan" / "io_summary_by_role.csv"),
                "artifact_dir": artifact(phase0 / "data" / "darshan"),
            },
            "perf": {
                "artifact_dir": artifact(phase0 / "raw" / "perf"),
                "status": "available_when_perf_profiles_exist_for_selected_run",
            },
            "linux_network_measurement_commands": {
                "summary": text_head(phase0 / "data" / "network" / "distributed-node-network-commands.txt", lines=80),
                "artifact_dir": artifact(phase0 / "raw" / "network"),
            },
            "ebpf_based_tools": {
                "status": "pending_admin_enablement_or_allowlisted_wrapper",
                "admin_request": artifact(phase0 / "ebpf-admin-command-allowlist.md"),
            },
        },
        "correctness": {
            "status": "phase0_basic_validation_only",
            "policy": "profileforge/validators/correctness-policy.md",
            "required_upgrade": "emit explicit no-lost-records/no-duplicates/ordering/range validator JSON before autonomous patch acceptance",
        },
        "deployment": {
            "topology_document": "profiling/chronolog-deployment-topology.md",
            "manifest": artifact(result_dir / "config" / "chronolog-config-manifest.env"),
        },
        "agent_guidance": {
            "diagnosis_contract": "profileforge/agents/bottleneck-diagnosis.md",
            "patch_contract": "profileforge/agents/patch-agent.md",
            "allowed_edits": "profileforge/targets/chronolog/allowed_edits.yaml",
            "acceptance_policy": "profileforge/controller/acceptance-policy.yaml",
        },
    }
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iteration", type=int, default=0)
    parser.add_argument("--output", default="")
    args = parser.parse_args()

    evidence = build_evidence(args.iteration)
    output = Path(args.output) if args.output else PROFILEFORGE_RESULTS / str(args.iteration) / "evidence.json"
    if not output.is_absolute():
        output = REPO_ROOT / output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(output.relative_to(REPO_ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
