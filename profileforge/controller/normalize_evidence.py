#!/usr/bin/env python3
"""Normalize ProfileForge iteration evidence into one agent-facing JSON file."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
PROFILING = REPO_ROOT / "profiling"
HISTORY = PROFILING / "data" / "history"
PROFILEFORGE_RESULTS = REPO_ROOT / "profileforge" / "results"
TAU_DURATION_RE = re.compile(
    r'"([^"]+_duration_us)"\s+(\d+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)\s+([0-9.eE+-]+)'
)


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


def display_path(path: Path) -> str:
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


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


def role_from_tau_profile(path: Path, tau_root: Path) -> str:
    rel = path.relative_to(tau_root)
    return "client" if rel.parts[0] == "client" else rel.parts[0]


def tau_summary_from_result(result_dir: Path, limit: int = 12) -> dict[str, Any]:
    tau_root = result_dir / "chronolog" / "profiles" / "tau"
    totals: dict[tuple[str, str], dict[str, Any]] = {}
    if not tau_root.exists():
        return {"source": artifact(tau_root), "top_regions": [], "status": "no_tau_profiles_found"}
    for profile_path in sorted(tau_root.glob("**/profile.*")):
        role = role_from_tau_profile(profile_path, tau_root)
        for line in profile_path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = TAU_DURATION_RE.match(line)
            if not match:
                continue
            region = match.group(1).replace("_duration_us", "")
            count = float(match.group(2))
            max_us = float(match.group(3))
            mean_us = float(match.group(5))
            row = totals.setdefault(
                (role, region),
                {"role": role, "region": region, "count": 0.0, "total_us": 0.0, "max_us": 0.0},
            )
            row["count"] += count
            row["total_us"] += count * mean_us
            row["max_us"] = max(row["max_us"], max_us)
    rows = sorted(totals.values(), key=lambda row: float(row["total_us"]), reverse=True)
    return {"source": artifact(tau_root), "top_regions": rows[:limit]}


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
    return display_path(path) if path.exists() else ""


def baseline_ratios_for(metrics: dict[str, Any]) -> dict[str, Any]:
    workflow = str(metrics.get("workflow", ""))
    node_count = int(metrics.get("node_count", 0) or 0)
    message_size = int(metrics.get("message_size_bytes", 0) or 0)
    throughput = float(metrics.get("throughput_ops_per_sec", 0) or 0)
    out: dict[str, Any] = {}

    for row in read_csv(HISTORY / "chronolog_metrics_history.csv"):
        if (
            row["workflow"] == workflow
            and int(row["node_count"]) == node_count
            and int(row["message_size_bytes"]) == message_size
        ):
            base = float(row["chronolog_throughput_ops_per_sec"])
            out["chronolog_to_iteration0"] = throughput / base if base else None
            out["chronolog_iteration0_metrics_path"] = row["metrics_path"]
            break

    for row in read_csv(HISTORY / "fixed_baselines.csv"):
        if (
            row["workflow"] == workflow
            and int(row["node_count"]) == node_count
            and int(row["message_size_bytes"]) == message_size
        ):
            baseline_metrics = read_json(repo_path(row["metrics_path"]))
            baseline_throughput = float(baseline_metrics.get("throughput_ops_per_sec", 0) or 0)
            out[f"chronolog_to_{row['system']}"] = throughput / baseline_throughput if baseline_throughput else None
            out[f"{row['system']}_baseline_metrics_path"] = row["metrics_path"]

    return out


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


def build_evidence_from_result(result_dir: Path, label: str = "candidate") -> dict[str, Any]:
    metrics_path = result_dir / "chronolog" / "metrics.json"
    if not metrics_path.exists():
        matches = sorted(result_dir.glob("**/chronolog/metrics.json"))
        if not matches:
            raise SystemExit(f"no ChronoLog metrics.json found under {result_dir}")
        metrics_path = matches[0]
        result_dir = metrics_path.parents[1]
    raw_metrics = read_json(metrics_path)
    correctness_path = result_dir / "correctness.json"

    phase0 = PROFILING / "0"
    return {
        "iteration": None,
        "timestamp": result_dir.name,
        "label": label,
        "commit": "",
        "target": "chronolog",
        "result_dir": display_path(result_dir),
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
            "metrics_path": display_path(metrics_path),
        },
        "baseline_ratios": baseline_ratios_for(raw_metrics),
        "profilers": {
            "tau": tau_summary_from_result(result_dir),
            "gperftools": {
                "artifact_dir": artifact(result_dir / "chronolog" / "profiles" / "gperftools"),
                "fallback_phase0_summary": csv_summary(phase0 / "data" / "gperftools" / "top_cpu_samples_by_role.csv"),
            },
            "darshan": {
                "artifact_dir": artifact(result_dir / "chronolog" / "profiles" / "darshan"),
                "fallback_phase0_summary": csv_summary(phase0 / "data" / "darshan" / "io_summary_by_role.csv"),
            },
            "perf": {
                "artifact_dir": artifact(result_dir / "chronolog" / "profiles" / "perf"),
                "status": "available_when_perf_profiles_exist_for_selected_run",
            },
            "linux_network_measurement_commands": {
                "artifact_dir": artifact(result_dir / "network"),
                "fallback_phase0_artifact_dir": artifact(phase0 / "raw" / "network"),
            },
            "ebpf_based_tools": {
                "status": "pending_admin_enablement_or_allowlisted_wrapper",
                "admin_request": artifact(phase0 / "ebpf-admin-command-allowlist.md"),
            },
        },
        "correctness": read_json(correctness_path) if correctness_path.exists() else {
            "status": "missing",
            "required": "run profileforge/validators/validate_correctness.py",
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iteration", type=int, default=0)
    parser.add_argument("--result-dir", default="")
    parser.add_argument("--label", default="candidate")
    parser.add_argument("--output", default="")
    args = parser.parse_args()

    if args.result_dir:
        evidence = build_evidence_from_result(repo_path(args.result_dir), args.label)
        output = Path(args.output) if args.output else repo_path(args.result_dir) / "evidence.json"
    else:
        evidence = build_evidence(args.iteration)
        output = Path(args.output) if args.output else PROFILEFORGE_RESULTS / str(args.iteration) / "evidence.json"
    if not output.is_absolute():
        output = REPO_ROOT / output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(display_path(output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
