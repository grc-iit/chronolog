#!/usr/bin/env python3
"""
compare_perf.py — Compare a perf-regression run against the previous release's
baseline.

Usage:
    compare_perf.py CURRENT.json --baselines-root DIR [--threshold 0.10]

Behavior (per tests/performance/perf_regression_design.md §5):
  • Loads CURRENT.json.
  • Finds the previous release: highest git tag matching
    v[0-9]+.[0-9]+.[0-9]+ strictly less than CURRENT.version.
  • Loads <baselines-root>/<prev>/ares/ofi+sockets/4groups_4x20clients/
    record_event.json.
  • Compares per event size for both record_event_bw_mbps and record_event_ops;
    prints a table per metric.
  • Exits 0 (pass) iff every (size, metric) pair is >= (1 - threshold) × baseline.
  • If no eligible previous baseline exists, exits 0 with a [BOOTSTRAP] note.

Pure stdlib — no external deps.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_THRESHOLD = 0.10  # 10 % regression cutoff

METRICS = [
    ("record_event_bw_mbps", "MB/s"),
    ("record_event_ops",     "ops/s"),
]


def semver_tuple(s: str) -> tuple[int, int, int]:
    return tuple(int(x) for x in s.split("."))  # type: ignore[return-value]


def previous_version(current: str) -> str | None:
    """Latest git tag matching v[0-9]+.[0-9]+.[0-9]+ strictly < current."""
    try:
        out = subprocess.check_output(
            ["git", "-C", str(REPO_ROOT), "tag", "--list", "v[0-9]*"],
            text=True,
        )
    except subprocess.CalledProcessError:
        return None

    tag_pat = re.compile(r"^v([0-9]+\.[0-9]+\.[0-9]+)$")
    cur = semver_tuple(current)
    versions = []
    for line in out.splitlines():
        m = tag_pat.match(line.strip())
        if not m:
            continue
        v = m.group(1)
        if semver_tuple(v) < cur:
            versions.append(v)
    if not versions:
        return None
    return max(versions, key=semver_tuple)


def baseline_path(root: Path, version: str) -> Path:
    return root / version / "ares" / "ofi+sockets" / "4groups_4x20clients" / "record_event.json"


def load_json(path: Path) -> dict:
    with path.open() as f:
        return json.load(f)


def format_table(
    rows: list[tuple[str, float, float | None, float | None, bool]],
    unit: str,
) -> str:
    """rows: (event_size_str, current, previous, pct_delta, passed)"""
    col = f"current {unit}"
    pcol = f"previous {unit}"
    out = [
        f"| event size | {col:>14s} | {pcol:>15s} |    Δ % | status |",
        f"|-----------:|{'-'*16}:|{'-'*17}:|-------:|:------:|",
    ]
    for esz, cur, prev, delta, ok in rows:
        prev_s  = f"{prev:.3f}"  if prev  is not None else "  —  "
        delta_s = f"{delta:+.2f}" if delta is not None else "  —  "
        marker = "✓" if ok else "✗"
        out.append(
            f"| {esz:>10s} | {cur:>16.3f} | {prev_s:>17s} | {delta_s:>6s} |   {marker}   |"
        )
    return "\n".join(out)


def compare_metric(
    cur_data: dict,
    prev_data: dict,
    metric_key: str,
    unit: str,
    threshold: float,
) -> tuple[bool, str]:
    """Compare one metric across all event sizes. Returns (any_failed, table_str)."""
    cur_vals  = {k: float(v) for k, v in cur_data["metrics"][metric_key].items()}
    prev_vals = {k: float(v) for k, v in prev_data["metrics"][metric_key].items()}

    rows: list[tuple[str, float, float | None, float | None, bool]] = []
    any_failed = False
    for esz in sorted(cur_vals, key=int):
        cur_v  = cur_vals[esz]
        prev_v = prev_vals.get(esz)
        if prev_v is None or prev_v <= 0:
            rows.append((esz, cur_v, prev_v, None, True))
            continue
        delta_pct = (cur_v / prev_v - 1.0) * 100.0
        ok = cur_v >= (1.0 - threshold) * prev_v
        rows.append((esz, cur_v, prev_v, delta_pct, ok))
        if not ok:
            any_failed = True

    return any_failed, format_table(rows, unit)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("current_json", type=Path)
    ap.add_argument("--baselines-root", required=True, type=Path,
                    help="Local path to a checkout of the perf-baselines branch.")
    ap.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                    help="Fractional regression cutoff (default 0.10 = 10%%).")
    args = ap.parse_args()

    current = load_json(args.current_json)
    cur_version = current["version"]

    prev_version_str = previous_version(cur_version)
    if prev_version_str is None:
        print(f"[BOOTSTRAP] No previous-version tag found before {cur_version}; "
              f"comparison passes by default.")
        return 0

    prev_path = baseline_path(args.baselines_root, prev_version_str)
    if not prev_path.exists():
        print(f"[BOOTSTRAP] No baseline at {prev_path}; comparison passes by default.")
        return 0

    prev = load_json(prev_path)

    print(f"Comparing {cur_version} ↔ {prev_version_str} "
          f"(threshold = {args.threshold * 100:.1f}% regression)")
    print()

    overall_failed = False
    for metric_key, unit in METRICS:
        any_failed, table = compare_metric(
            current, prev, metric_key, unit, args.threshold
        )
        print(f"### {metric_key} ({unit})")
        print()
        print(table)
        print()
        if any_failed:
            overall_failed = True

    if overall_failed:
        print(f"FAIL: at least one (event size, metric) pair regressed by "
              f"≥ {args.threshold * 100:.0f}% vs {prev_version_str}.")
        return 1

    print(f"PASS: no metric regressed by ≥ {args.threshold * 100:.0f}% "
          f"vs {prev_version_str}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
