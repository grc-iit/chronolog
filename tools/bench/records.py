#!/usr/bin/env python3
"""Turns benchmark output into JSON lines that carry the metadata record.

  records.py meta --binaries DIR --wal-dir DIR --quick 0|1 --build-lock NAME --perf 0|1
  records.py gbench --in google-benchmark.json --meta meta.json
  records.py wrap --raw raw.jsonl --meta meta.json
  records.py profile --data perf.data --workload NAME --meta meta.json
"""
import argparse
import json
import os
import platform
import re
import socket
import subprocess
import sys
from datetime import datetime, timezone


def cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def cmake_cache(binaries, key):
    try:
        with open(os.path.join(binaries, "CMakeCache.txt")) as f:
            for line in f:
                if line.startswith(key + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return "unknown"


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return "unknown"


def meta(args):
    fs = subprocess.run(["stat", "-f", "-c", "%T", args.wal_dir], capture_output=True, text=True).stdout.strip()
    device = subprocess.run(["df", "--output=source", args.wal_dir], capture_output=True, text=True).stdout.split("\n")[-2:-1]
    print(json.dumps({
        "commit": os.environ.get("BENCH_COMMIT", "unknown"),
        "build_type": cmake_cache(args.binaries, "CMAKE_BUILD_TYPE"),
        "cxx_flags": cmake_cache(args.binaries, "CMAKE_CXX_FLAGS"),
        "host": socket.gethostname(),
        "cpu_model": cpu_model(),
        "cpu_count": os.cpu_count(),
        "kernel": platform.release(),
        "wal_filesystem": fs or "unknown",
        "wal_device": device[0].strip() if device else "unknown",
        "build_lock_held": args.build_lock == "build",
        "reduced_parameters": args.quick == "1",
        "perf_recording": args.perf == "1",
        "perf_event_paranoid": read("/proc/sys/kernel/perf_event_paranoid"),
        "date_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }))


def load_meta(path):
    with open(path) as f:
        return json.load(f)


def gbench(args):
    data = load_meta(args.meta)
    with open(args.__dict__["in"]) as f:
        run = json.load(f)
    context = run.get("context", {})
    for b in run.get("benchmarks", []):
        counters = {k: v for k, v in b.items() if k not in (
            "name", "family_index", "per_family_instance_index", "run_name", "run_type", "repetitions",
            "repetition_index", "threads", "iterations", "real_time", "cpu_time", "time_unit", "label",
            "bytes_per_second", "items_per_second", "error_occurred", "error_message")}
        print(json.dumps({
            "layer": "micro",
            "suite": b["name"].split("/")[0],
            "params": {"name": b["name"], "label": b.get("label", ""), "threads": b.get("threads", 1)},
            "result": {"real_time": b["real_time"], "cpu_time": b["cpu_time"], "time_unit": b["time_unit"],
                       "iterations": b["iterations"], "bytes_per_second": b.get("bytes_per_second"),
                       "items_per_second": b.get("items_per_second"), "counters": counters,
                       "error": b.get("error_message")},
            "benchmark_context": {"library_build_type": context.get("library_build_type"),
                                  "cpu_scaling_enabled": context.get("cpu_scaling_enabled")},
            "meta": data}))


def wrap(args):
    data = load_meta(args.meta)
    with open(args.raw) as f:
        for line in f:
            if line.strip():
                record = json.loads(line)
                record["meta"] = data
                print(json.dumps(record))


BUCKETS = [
    ("fdatasync_and_block_io", r"fdatasync|fsync|ext4|jbd2|blk_|nvme|io_schedule|vfs_write|__x64_sys_(p?write|fdatasync)|generic_perform_write|submit_bio"),
    ("checksum", r"crc32|Crc32"),
    ("hdf5", r"\bH5|hdf5"),
    ("serialization", r"google::protobuf|protobuf|_InternalSerialize|ByteSizeLong|MergeFrom|ParseFrom|Serialize|Parse|upb_|nlohmann"),
    ("grpc_and_http2", r"grpc|chttp2|hpack|http2|promise|Arena|EventEngine|iomgr|ev_|absl::.*(Cord|Status)"),
    ("network_and_syscalls", r"epoll|tcp_|sock|inet|loopback|net_rx|__sys_(send|recv)|__x64_sys|entry_SYSCALL|do_syscall|skb|ip_|__dev_"),
    ("locks_and_scheduling", r"mutex|[Ll]ock|futex|pthread_cond|spin|sched|schedule|__lll|Mutex|wake|resched"),
    ("allocation", r"malloc|free\b|operator new|operator delete|_int_|tcache|brk|mmap|page_fault|clear_page|alloc_pages|memcg|zap_pte|handle_mm_fault|asm_exc_page_fault"),
    ("memcpy_and_memset", r"memcpy|memmove|memset|__copy|copy_user|rep_movs"),
]


def bucket(symbol):
    for name, pattern in BUCKETS:
        if re.search(pattern, symbol):
            return name
    return "other"


LINE = re.compile(r"^\s*(\d+\.\d+)%\s+(\S+)\s+(\S+)\s+\[(.)\]\s+(.+?)\s*$")


def profile(args):
    data = load_meta(args.meta)
    report = subprocess.run(["perf", "report", "-i", args.data, "--no-children", "--stdio", "-g", "none",
                             "--sort", "comm,dso,sym", "--percent-limit", "0.05"],
                            capture_output=True, text=True)
    if report.returncode != 0:
        sys.stderr.write(report.stderr)
        sys.exit(1)
    rows = []
    for line in report.stdout.splitlines():
        m = LINE.match(line)
        if m:
            rows.append({"percent": float(m.group(1)), "process": m.group(2), "dso": m.group(3),
                         "kernel": m.group(4) == "k", "symbol": m.group(5)})
    by_process = {}
    for row in rows:
        row["bucket"] = bucket(row["symbol"])
        cell = by_process.setdefault(row["process"], {})
        cell[row["bucket"]] = round(cell.get(row["bucket"], 0.0) + row["percent"], 2)
    top = sorted(rows, key=lambda r: -r["percent"])[:12]
    out_dir = os.path.dirname(args.meta)
    names = sorted(by_process, key=lambda p: -sum(by_process[p].values()))
    buckets = [b for b, _ in BUCKETS] + ["other"]
    lines = [f"profile {args.workload}: share of all samples in the service processes (perf -F 499 -g, frame pointers)"]
    lines.append("%-24s" % "bucket" + "".join("%16s" % n for n in names))
    for b in buckets:
        lines.append("%-24s" % b + "".join("%15.1f%%" % by_process[n].get(b, 0.0) for n in names))
    lines.append("top symbols:")
    for r in top:
        lines.append("  %5.1f%%  %-16s %-10s %s" % (r["percent"], r["process"], r["bucket"], r["symbol"][:90]))
    with open(os.path.join(out_dir, f"profile-{args.workload}.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(json.dumps({"layer": "profile", "suite": args.workload, "params": {"sample_hz": 499},
                      "result": {"by_process_and_bucket_percent": by_process,
                                 "top_symbols": [{k: r[k] for k in ("percent", "process", "bucket", "symbol")} for r in top]},
                      "meta": data}))


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("meta")
    for a in ("binaries", "wal_dir", "quick", "build_lock", "perf"):
        p.add_argument("--" + a.replace("_", "-"), dest=a, required=True)
    p = sub.add_parser("gbench")
    p.add_argument("--in", dest="in", required=True)
    p.add_argument("--meta", required=True)
    p = sub.add_parser("wrap")
    p.add_argument("--raw", required=True)
    p.add_argument("--meta", required=True)
    p = sub.add_parser("profile")
    p.add_argument("--data", required=True)
    p.add_argument("--workload", required=True)
    p.add_argument("--meta", required=True)
    args = parser.parse_args()
    {"meta": meta, "gbench": gbench, "wrap": wrap, "profile": profile}[args.command](args)


if __name__ == "__main__":
    main()
