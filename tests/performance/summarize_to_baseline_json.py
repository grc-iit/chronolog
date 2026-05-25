#!/usr/bin/env python3
"""
summarize_to_baseline_json.py — Reduce an ares_test_logs_* run to a single
per-version baseline JSON.

Inputs:
    --logs-dir DIR   Path to an ares_test_logs_<stamp>/ produced by
                     ares_test.sh; must contain by_component_scale/
                     recording_*_20x4_esz*.csv files (extract_plot_results.py
                     output).
    --out PATH       Destination JSON file.

Schema (see tests/performance/perf_regression_design.md §3):
    {
      "schema_version": 2,
      "version": "<X.Y.Z>",          parsed from CMakeLists.txt
      "git_sha":  "<sha>",           current HEAD
      "test_date": "<ISO 8601 UTC>",
      "platform": "ares",
      "config": { ...frozen narrow case... },
      "reps": 3,
      "aggregation": "mean"|"median", (mean when REPS<=5, median when REPS>5)
      "metrics": {
        "record_event_bw_mbps": { "<event_size>": <bw>, ... },
        "record_event_ops":     { "<event_size>": <ops>, ... }
      }
    }
"""

import argparse
import csv
import datetime
import json
import os
import re
import statistics
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def parse_version_from_cmake() -> str:
    cml = REPO_ROOT / "CMakeLists.txt"
    pat = re.compile(r"^\s*project\([^)]*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", re.M)
    m = pat.search(cml.read_text())
    if not m:
        sys.exit(f"ERROR: could not parse VERSION from {cml}")
    return m.group(1)


def git_sha() -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
            text=True,
        ).strip()
    except subprocess.CalledProcessError:
        return ""


def load_recording_csv(
    path: Path, component_scale: int = 4
) -> tuple[float | None, float | None]:
    """Return (mean_record_bandwidth_mbs, mean_record_events_per_s) for scale=4.

    extract_plot_results.py groups by component_scale; the regression case
    runs only with scale=4, so we pick that row directly.
    """
    with path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            if int(row["component_scale"]) == component_scale:
                bw  = row.get("mean_record_bandwidth_mbs")
                ops = row.get("mean_record_events_per_s")
                return (
                    float(bw)  if bw  not in (None, "") else None,
                    float(ops) if ops not in (None, "") else None,
                )
    return None, None


def collect_results(
    logs_dir: Path,
) -> tuple[dict[str, float], dict[str, float]]:
    """Walk by_component_scale/recording_ofi_sockets_20x4_esz*.csv and pull
    out bandwidth (MB/s) and throughput (events/s) per event size."""
    by_dir = logs_dir / "by_component_scale"
    if not by_dir.is_dir():
        sys.exit(f"ERROR: missing {by_dir} — did extract_plot_results.py run?")

    pattern = re.compile(r"^recording_ofi_sockets_20x4_esz(\d+)\.csv$")
    bw_out:  dict[str, float] = {}
    ops_out: dict[str, float] = {}
    for entry in sorted(by_dir.iterdir()):
        m = pattern.match(entry.name)
        if not m:
            continue
        esz = m.group(1)
        bw, ops = load_recording_csv(entry)
        if bw  is not None:
            bw_out[esz]  = bw
        if ops is not None:
            ops_out[esz] = ops

    if not bw_out:
        sys.exit(f"ERROR: no recording_ofi_sockets_20x4_esz*.csv files in {by_dir}")
    return bw_out, ops_out


# Patterns matching chrono-bench stdout lines (same as extract_plot_results.py).
_RECORD_BW_PAT  = re.compile(
    r"^Record-event \(incl\. metadata time\) bandwidth:\s+([\d.eE+\-]+)\s+MB/s"
)
_RECORD_OPS_PAT = re.compile(
    r"^Record-event \(incl\. metadata time\) throughput:\s+([\d.eE+\-]+)\s+events/s"
)
# CSV result line written by ares_test.sh: test,proto,scale,clients,esz,ecnt,rep,STATUS,Ns,test=...
_CSV_RESULT_RE  = re.compile(
    r"^([a-z_]+),[^,]+,\d+,[^,]+,(\d+),\d+,\d+,[A-Z][A-Z_()\d]*,\d+s,test="
)


def collect_per_rep_from_log(
    log_path: Path,
) -> tuple[dict[str, list[float]], dict[str, list[float]]]:
    """Parse ares_test log and return per-esz lists of per-rep summed values.

    Each MPI process emits its own metric lines; they are summed to get the
    total system bandwidth/throughput for that rep, matching the behaviour of
    extract_plot_results.py.
    """
    bw_reps:  dict[str, list[float]] = {}
    ops_reps: dict[str, list[float]] = {}
    cur_bw:   list[float] = []
    cur_ops:  list[float] = []

    with log_path.open() as f:
        for raw in f:
            line = raw.rstrip("\n")
            m = _RECORD_BW_PAT.match(line)
            if m:
                cur_bw.append(float(m.group(1)))
                continue
            m = _RECORD_OPS_PAT.match(line)
            if m:
                cur_ops.append(float(m.group(1)))
                continue
            m = _CSV_RESULT_RE.match(line)
            if m and m.group(1) == "recording":
                esz = m.group(2)
                if cur_bw:
                    bw_reps.setdefault(esz, []).append(sum(cur_bw))
                if cur_ops:
                    ops_reps.setdefault(esz, []).append(sum(cur_ops))
                cur_bw  = []
                cur_ops = []

    return bw_reps, ops_reps


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs-dir", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    logs_dir = Path(args.logs_dir).resolve()
    out_path = Path(args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    reps = int(os.environ.get("REPS", "3"))

    def sort_by_esz(d: dict[str, float]) -> dict[str, float]:
        return {k: round(v, 4) for k, v in sorted(d.items(), key=lambda kv: int(kv[0]))}

    if reps > 5:
        log_files = sorted(logs_dir.glob("ares_test_*.log"))
        if not log_files:
            print(
                "WARNING: REPS>5 but no ares_test_*.log found in logs dir "
                "— falling back to mean from CSVs",
                file=sys.stderr,
            )
            reps = 5  # fall through to mean path
        else:
            bw_per_rep, ops_per_rep = collect_per_rep_from_log(log_files[0])
            if not bw_per_rep:
                sys.exit("ERROR: log parsing found no recording metrics")
            bw_results  = {esz: statistics.median(v) for esz, v in bw_per_rep.items()}
            ops_results = {esz: statistics.median(v) for esz, v in ops_per_rep.items()}
            aggregation = "median"

    if reps <= 5:
        bw_results, ops_results = collect_results(logs_dir)
        aggregation = "mean"

    doc = {
        "schema_version": 2,
        "version": parse_version_from_cmake(),
        "git_sha": git_sha(),
        "test_date": datetime.datetime.now(datetime.timezone.utc)
                              .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "platform": "ares",
        "config": {
            "recording_groups": 4,
            "keepers_per_group": 1,
            "graphers_per_group": 1,
            "players_per_group": 1,
            "client_nodes": 4,
            "clients_per_node": 20,
            "protocol": "ofi+sockets",
            "ingestion_thread_count": {
                "keeper": "default",
                "grapher": "default",
                "player": "default",
            },
        },
        "reps": reps,
        "aggregation": aggregation,
        "metrics": {
            "record_event_bw_mbps": sort_by_esz(bw_results),
            "record_event_ops":     sort_by_esz(ops_results),
        },
    }

    with out_path.open("w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"[summarize] wrote {out_path}")


if __name__ == "__main__":
    main()
