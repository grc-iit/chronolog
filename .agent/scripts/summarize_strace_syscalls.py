#!/usr/bin/env python3
"""Summarize syscall timing from strace -T output files."""

from __future__ import annotations

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path


SYSCALL_RE = re.compile(r"^\d+(?:\.\d+)?\s+([A-Za-z0-9_]+)\(")
ELAPSED_RE = re.compile(r"<([0-9]+(?:\.[0-9]+)?)>\s*$")


def parse_file(path: Path) -> dict[str, dict[str, float]]:
    stats: dict[str, dict[str, float]] = defaultdict(
        lambda: {"calls": 0, "seconds": 0.0, "max_seconds": 0.0}
    )
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            syscall_match = SYSCALL_RE.match(line)
            elapsed_match = ELAPSED_RE.search(line)
            if not syscall_match or not elapsed_match:
                continue
            syscall = syscall_match.group(1)
            elapsed = float(elapsed_match.group(1))
            entry = stats[syscall]
            entry["calls"] += 1
            entry["seconds"] += elapsed
            entry["max_seconds"] = max(entry["max_seconds"], elapsed)
    return dict(stats)


def merge_stats(paths: list[Path]) -> dict[str, dict[str, float]]:
    merged: dict[str, dict[str, float]] = defaultdict(
        lambda: {"calls": 0, "seconds": 0.0, "max_seconds": 0.0}
    )
    for path in paths:
        for syscall, values in parse_file(path).items():
            entry = merged[syscall]
            entry["calls"] += values["calls"]
            entry["seconds"] += values["seconds"]
            entry["max_seconds"] = max(entry["max_seconds"], values["max_seconds"])
    for values in merged.values():
        calls = values["calls"]
        values["avg_us"] = (values["seconds"] / calls * 1_000_000.0) if calls else 0.0
        values["max_us"] = values["max_seconds"] * 1_000_000.0
    return dict(merged)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path, help="strace files or directories")
    parser.add_argument("--top", type=int, default=25)
    parser.add_argument("--json-out", type=Path, default=None)
    args = parser.parse_args()

    files: list[Path] = []
    for path in args.paths:
        if path.is_dir():
            files.extend(sorted(item for item in path.rglob("*") if item.is_file()))
        elif path.is_file():
            files.append(path)

    stats = merge_stats(files)
    rows = sorted(stats.items(), key=lambda item: item[1]["seconds"], reverse=True)
    summary = {
        "file_count": len(files),
        "syscalls": stats,
        "top_by_seconds": [
            {
                "syscall": syscall,
                "calls": int(values["calls"]),
                "seconds": values["seconds"],
                "avg_us": values["avg_us"],
                "max_us": values["max_us"],
            }
            for syscall, values in rows[: args.top]
        ],
    }
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    for row in summary["top_by_seconds"]:
        print(
            f"{row['syscall']:24s} calls={row['calls']:9d} "
            f"seconds={row['seconds']:12.6f} avg_us={row['avg_us']:10.3f} max_us={row['max_us']:10.3f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
