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
    fs = subprocess.run(["findmnt", "-no", "FSTYPE", "-T", args.wal_dir], capture_output=True, text=True).stdout.strip()
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
    ("fdatasync_and_block_io", r"fdatasync|fsync|ext4|jbd2|blk_|nvme|io_schedule|vfs_write|generic_perform_write|submit_bio"),
    ("checksum", r"crc32|Crc32"),
    ("hdf5", r"\bH5|hdf5"),
    ("serialization", r"google::protobuf|protobuf|_InternalSerialize|ByteSizeLong|MergeFrom|ParseFrom|Serialize|nlohmann|upb_"),
    ("grpc_and_http2", r"grpc|chttp2|hpack|http2|promise|EventEngine|iomgr"),
    ("network_stack", r"epoll|tcp_|sock|inet|loopback|net_rx|__sys_(send|recv)|skb|ip_|__dev_|_copy_to_iter|_copy_from_iter"),
    ("locks_and_scheduling", r"mutex|[Ll]ock|futex|pthread_cond|spin|sched|schedule|__lll|Mutex|wake|resched|dequeue_|enqueue_|psi_group"),
    ("allocation_and_page_faults", r"malloc|free\b|cfree|operator new|operator delete|_int_|tcache|brk|mmap|page_fault|clear_page|alloc_pages|memcg|zap_pte|handle_mm_fault"),
    ("memcpy_and_memset", r"memcpy|memmove|memset|__copy|copy_user|rep_movs"),
    ("syscall_entry_and_mitigations", r"entry_SYS|srso|__irqentry|do_syscall|ret_from_fork|syscall_exit|swapgs|retbleed|spec_"),
]


def bucket(symbol):
    for name, pattern in BUCKETS:
        if re.search(pattern, symbol):
            return name
    return "other"


LINE = re.compile(r"^\s*(\d+\.\d+)%\s+(\S+)\s+\[(.)\]\s+(.+?)\s*$")


def one_role(path):
    report = subprocess.run(["perf", "report", "-i", path, "--no-children", "--stdio", "-g", "none",
                             "--sort", "dso,sym", "--percent-limit", "0"], capture_output=True, text=True)
    if report.returncode != 0:
        raise RuntimeError(report.stderr.strip())
    rows = []
    for line in report.stdout.splitlines():
        m = LINE.match(line)
        if m:
            rows.append({"percent": float(m.group(1)), "dso": m.group(2), "kernel": m.group(3) == "k",
                         "symbol": m.group(4)})
    samples = 0
    for line in report.stdout.splitlines():
        m = re.match(r"# Samples: ([\d.]+)([KMG]?)", line)
        if m:
            samples = int(float(m.group(1)) * {"": 1, "K": 1000, "M": 1000000, "G": 1000000000}[m.group(2)])
            break
    buckets = {}
    for row in rows:
        row["bucket"] = bucket(row["symbol"])
        buckets[row["bucket"]] = buckets.get(row["bucket"], 0.0) + row["percent"]
    return samples, rows, buckets


def profile(args):
    data = load_meta(args.meta)
    roles = {}
    for item in args.data.split(","):
        role, path = item.split("=", 1)
        try:
            roles[role] = one_role(path)
        except (RuntimeError, OSError) as error:
            sys.stderr.write(f"{role}: {error}\n")
    if not roles:
        sys.exit(1)
    total = sum(s for s, _, _ in roles.values()) or 1
    names = sorted(roles, key=lambda r: -roles[r][0])
    order = [b for b, _ in BUCKETS] + ["other"]
    lines = [f"profile {args.workload}: perf -F 499 -g per service process, percent of that process's own samples"]
    lines.append("%-30s" % "cpu samples" + "".join("%12d" % roles[n][0] for n in names))
    lines.append("%-30s" % "share of all samples" + "".join("%11.0f%%" % (100.0 * roles[n][0] / total) for n in names))
    lines.append("%-30s" % "bucket" + "".join("%12s" % n for n in names))
    for b in order:
        lines.append("%-30s" % b + "".join("%11.1f%%" % roles[n][2].get(b, 0.0) for n in names))
    top = {}
    for n in names:
        top[n] = sorted(roles[n][1], key=lambda r: -r["percent"])[:6]
        lines.append(f"top symbols of {n}:")
        for r in top[n]:
            lines.append("  %5.1f%%  %-26s %s" % (r["percent"], r["bucket"], r["symbol"][:96]))
    out_dir = os.path.dirname(args.meta)
    with open(os.path.join(out_dir, f"profile-{args.workload}.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print(json.dumps({"layer": "profile", "suite": args.workload, "params": {"sample_hz": 499},
                      "result": {"samples": {n: roles[n][0] for n in names},
                                 "bucket_percent_of_process": {n: {k: round(v, 2) for k, v in roles[n][2].items()} for n in names},
                                 "top_symbols": {n: [{k: r[k] for k in ("percent", "bucket", "symbol")} for r in top[n]] for n in names}},
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
