#!/usr/bin/env python3
"""
ChronoKeeper performance regression test suite.

Modes:
  run      Sweep #clients × event_sizes, invoke chronolog-test-performance -p -w,
           and log raw output to a timestamped log file.
  analyze  Parse an existing log file, extract perf metrics into CSV,
           and plot end-to-end bandwidth.

Events per client are computed automatically so that total data volume per
run stays roughly constant regardless of event size or client count.
Target volume is ~600 MB for small events, ~1200 MB for event_size >= 4096,
scaled by the number of nodes when --hostfile is provided.
This keeps run times comparable and avoids excessive memory/time for large
payloads:
    events_per_client = TARGET_BYTES * num_nodes / (event_size * num_clients)
    clamped to [1000, 200000]

Usage:
  # Run a sweep (starts/stops ChronoLog automatically):
  python3 perf_regression.py run \\
      --install-dir ~/chronolog-install \\
      --clients 1 2 4 8 16 \\
      --event-sizes 64 256 1024 4096

  # Analyze a previous run:
  python3 perf_regression.py analyze \\
      --log results/perf_20260325_143012.log

  # Multi-node run with a hostfile:
  python3 perf_regression.py run --hostfile hosts.txt --clients 8 16 32 ...

  # Run with 3 repetitions per config for averaging:
  python3 perf_regression.py run --reps 3 ...

  # Both in one shot:
  python3 perf_regression.py run --analyze ...
"""

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent
RESULTS_DIR = SCRIPT_DIR / "results"

# Target total data volume per run (~600 MB).  events_per_client is derived
# so that  event_size * num_clients * events_per_client ≈ TARGET_BYTES.
TARGET_BYTES_PER_RUN = 600 * 1024 * 1024  # 600 MB (small events)
TARGET_BYTES_PER_RUN_LARGE = 1200 * 1024 * 1024  # 1200 MB (event_size >= 4096)
MIN_EVENTS_PER_CLIENT = 1000
MAX_EVENTS_PER_CLIENT = 200000

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def find_mpiexec(install_dir: Path) -> str:
    """Locate mpiexec — prefer the spack-env view bundled with the repo."""
    repo_root = SCRIPT_DIR.parent.parent
    spack_mpiexec = repo_root / ".spack-env" / "view" / "bin" / "mpiexec"
    if spack_mpiexec.is_file() and os.access(spack_mpiexec, os.X_OK):
        return str(spack_mpiexec)
    mpiexec = shutil.which("mpiexec")
    if mpiexec:
        return mpiexec
    sys.exit("ERROR: mpiexec not found.")


def find_binary(install_dir: Path, name: str) -> str:
    path = install_dir / "chronolog" / "bin" / name
    if not path.is_file():
        sys.exit(f"ERROR: {path} not found.")
    return str(path)


def timestamp_str() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def _round_to_nice(n: int) -> int:
    """Round n to the nearest value in the sequence 1000, 5000, 10000, 50000, 100000, 500000, ..."""
    if n <= 1000:
        return 1000
    # Build candidates: powers of 10 and 5×powers of 10
    candidates = []
    p = 1000
    while p <= 10_000_000:
        candidates.append(p)
        candidates.append(5 * p)
        p *= 10
    # Pick the closest candidate
    return min(candidates, key=lambda c: abs(c - n))


def count_hostfile_nodes(hostfile: str) -> int:
    """Count the number of unique non-empty, non-comment lines in a hostfile."""
    hosts = set()
    with open(hostfile) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                # Handle "host slots=N" or "host:N" formats — extract hostname
                hosts.add(line.split()[0].split(":")[0])
    return max(1, len(hosts))


def compute_events_per_client(event_size: int, num_clients: int, num_nodes: int = 1) -> int:
    """Derive events_per_client so total volume ≈ TARGET_BYTES_PER_RUN * num_nodes."""
    target = TARGET_BYTES_PER_RUN_LARGE if event_size >= 4096 else TARGET_BYTES_PER_RUN
    target *= num_nodes
    n = target // (event_size * num_clients)
    n = _round_to_nice(n)
    return max(MIN_EVENTS_PER_CLIENT, min(MAX_EVENTS_PER_CLIENT, n))


# JSON paths (dot-separated keys) whose protocol_conf should be switched.
# All other protocol_conf fields stay ofi+sockets.
SWITCHABLE_PROTOCOL_PATHS = [
    ("chrono_keeper", "KeeperRecordingService", "rpc", "protocol_conf"),
    ("chrono_keeper", "KeeperGrapherDrainService", "rpc", "protocol_conf"),
    ("chrono_grapher", "KeeperGrapherDrainService", "rpc", "protocol_conf"),
]


def patch_protocol_conf(conf_path: str, protocol: str):
    """Patch default-chrono-conf.json: set the 3 switchable protocol_conf
    fields to *protocol* and ensure all others are ofi+sockets."""
    with open(conf_path) as f:
        conf = json.load(f)

    # Set the 3 switchable paths to the chosen protocol
    for keys in SWITCHABLE_PROTOCOL_PATHS:
        node = conf
        for k in keys[:-1]:
            node = node[k]
        node[keys[-1]] = protocol

    # Walk the entire config and force every OTHER protocol_conf to ofi+sockets
    def _force_sockets(obj, path=()):
        if isinstance(obj, dict):
            for k, v in obj.items():
                current_path = path + (k,)
                if (
                    k == "protocol_conf"
                    and current_path not in SWITCHABLE_PROTOCOL_PATHS
                ):
                    obj[k] = "ofi+sockets"
                else:
                    _force_sockets(v, current_path)
        elif isinstance(obj, list):
            for item in obj:
                _force_sockets(item, path)

    _force_sockets(conf)

    with open(conf_path, "w") as f:
        json.dump(conf, f, indent=4)
        f.write("\n")
    print(f"  Patched protocol_conf → {protocol} (switchable), ofi+sockets (others)")


# ---------------------------------------------------------------------------
# Run mode
# ---------------------------------------------------------------------------


def deploy_start(deploy_script: str, work_dir: str, keepers: int):
    print(f"  Starting ChronoLog (keepers={keepers}) ...")
    subprocess.run(
        [deploy_script, "-d", "-k", str(keepers), "-w", work_dir],
        check=True,
    )
    time.sleep(4)


def deploy_stop(deploy_script: str, work_dir: str):
    print("  Stopping ChronoLog ...")
    subprocess.run([deploy_script, "-s", "-w", work_dir], check=False)
    time.sleep(3)
    subprocess.run([deploy_script, "-c", "-w", work_dir], check=False)


def run_workload(
    mpiexec: str,
    client_bin: str,
    client_conf: str,
    num_clients: int,
    event_count: int,
    event_size: int,
    story_count: int,
    hostfile: str = None,
) -> tuple[str, str]:
    """Run chronolog-test-performance and return (command_line, combined stdout+stderr)."""
    cmd = [mpiexec]
    if hostfile:
        cmd += ["-hostfile", hostfile]
    cmd += [
        "-n",
        str(num_clients),
        client_bin,
        "-c",
        client_conf,
        "-w",  # write mode
        "-n",
        str(event_count),
        "-t",
        str(story_count),
        "-h",
        "1",  # 1 chronicle
        "-a",
        str(event_size),
        "-s",
        str(event_size),
        "-b",
        str(event_size),
        "-p",  # perf reporting
    ]
    cmd_str = " ".join(cmd)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    return cmd_str, result.stdout + result.stderr


def run_sweep(args):
    install_dir = Path(args.install_dir).expanduser().resolve()
    work_dir = str(install_dir / "chronolog")
    deploy_script = os.path.join(work_dir, "tools", "deploy", "deploy_local.sh")
    mpiexec = find_mpiexec(install_dir)
    client_bin_path = install_dir / "chronolog" / "tools" / "benchmark" / "chrono-bench"
    if not client_bin_path.is_file():
        sys.exit(f"ERROR: {client_bin_path} not found.")
    client_bin = str(client_bin_path)
    client_conf = os.path.join(work_dir, "conf", "default-chrono-client-conf.json")
    hostfile = args.hostfile
    num_nodes = count_hostfile_nodes(hostfile) if hostfile else 1

    # Patch protocol_conf in the server config before any deployment
    server_conf = os.path.join(work_dir, "conf", "default-chrono-conf.json")
    patch_protocol_conf(server_conf, args.protocol)

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    log_name = f"perf_{timestamp_str()}.log"
    log_path = RESULTS_DIR / log_name
    print(f"Logging to: {log_path}")

    with open(log_path, "w") as log:
        header = (
            f"# ChronoKeeper perf regression — {datetime.now().isoformat()}\n"
            f"# clients: {args.clients}\n"
            f"# event_sizes: {args.event_sizes}\n"
            f"# target_bytes_per_run: {TARGET_BYTES_PER_RUN}\n"
            f"# stories: {args.stories}\n"
            f"# keepers: {args.keepers}\n"
            f"# hostfile: {hostfile or '(local)'}\n"
            f"# num_nodes: {num_nodes}\n"
            f"# reps: {args.reps}\n"
            f"# protocol: {args.protocol}\n\n"
        )
        log.write(header)
        print(header, end="")

        for event_size in args.event_sizes:
            for num_clients in args.clients:
                events_per_client = compute_events_per_client(event_size, num_clients, num_nodes)
                for rep in range(1, args.reps + 1):
                    tag = f"clients={num_clients} event_size={event_size}"
                    separator = f"{'=' * 70}\n"
                    run_header = (
                        f"{separator}"
                        f"[RUN] {tag} events_per_client={events_per_client} "
                        f"stories={args.stories} rep={rep}\n"
                        f"{separator}"
                    )
                    print(run_header, end="")
                    log.write(run_header)

                    # Start ChronoLog fresh for each run
                    deploy_start(deploy_script, work_dir, args.keepers)

                    try:
                        cmd_str, output = run_workload(
                            mpiexec,
                            client_bin,
                            client_conf,
                            num_clients,
                            events_per_client,
                            event_size,
                            args.stories,
                            hostfile=hostfile,
                        )
                    except subprocess.TimeoutExpired:
                        cmd_str, output = "", "ERROR: workload timed out after 600s\n"
                    except subprocess.SubprocessError as e:
                        cmd_str, output = "", f"ERROR: {e}\n"

                    log.write(f"[CMD] {cmd_str}\n")
                    log.write(output)
                    log.write("\n")
                    log.flush()
                    print(output[:500])  # preview

                    deploy_stop(deploy_script, work_dir)

    print(f"\nSweep complete. Log: {log_path}")
    return str(log_path)


# ---------------------------------------------------------------------------
# Analyze mode
# ---------------------------------------------------------------------------

# Patterns to extract from chronolog-test-performance -p output.
# Each line that matches is emitted by one MPI rank; we collect all of them
# per run block and report the aggregate (sum for bandwidth, per-rank for
# throughput).
METRIC_PATTERNS = {
    "total_payload_bytes": re.compile(r"Total payload written:\s+([\d.]+)\s+bytes"),
    "e2e_bw_MB_s": re.compile(r"End-to-end.*bandwidth:\s+([\d.]+)\s+MB/s"),
    "record_bw_MB_s": re.compile(r"Record-event.*bandwidth:\s+([\d.]+)\s+MB/s"),
    "record_throughput_ev_s": re.compile(
        r"Record-event.*throughput:\s+([\d.]+)\s+events/s"
    ),
}


def parse_log(log_path: str):
    """
    Parse a sweep log file into a list of dicts, one per run block.

    Each dict contains:
      clients, event_size, events_per_client, stories  (from the [RUN] header)
      e2e_bw_MB_s, record_bw_MB_s, ...                 (aggregated metrics)
    """
    run_header_re = re.compile(
        r"\[RUN\]\s+clients=(\d+)\s+event_size=(\d+)\s+"
        r"events_per_client=(\d+)\s+stories=(\d+)(?:\s+rep=(\d+))?"
    )
    results = []
    current_run = None

    with open(log_path) as f:
        for line in f:
            m = run_header_re.search(line)
            if m:
                if current_run is not None:
                    _finalize_run(current_run)
                    results.append(current_run)
                current_run = {
                    "clients": int(m.group(1)),
                    "event_size": int(m.group(2)),
                    "events_per_client": int(m.group(3)),
                    "stories": int(m.group(4)),
                    "_raw": {k: [] for k in METRIC_PATTERNS},
                }
                continue

            if current_run is None:
                continue

            for key, pat in METRIC_PATTERNS.items():
                mm = pat.search(line)
                if mm:
                    current_run["_raw"][key].append(float(mm.group(1)))

    if current_run is not None:
        _finalize_run(current_run)
        results.append(current_run)

    return results


def _finalize_run(run: dict):
    """Aggregate per-rank metrics into a single value per run."""
    raw = run.pop("_raw")
    for key, values in raw.items():
        if not values:
            run[key] = None
        elif "bw" in key:
            # Bandwidth: sum across ranks (each rank reports its own share)
            run[key] = sum(values)
        elif "throughput" in key:
            run[key] = sum(values)
        else:
            # total_payload_bytes: sum across ranks
            run[key] = sum(values)


METRIC_KEYS = ["total_payload_bytes", "e2e_bw_MB_s", "record_bw_MB_s", "record_throughput_ev_s"]


def _average_repetitions(results: list) -> list:
    """Average metric values across repetitions sharing the same config key."""
    from collections import OrderedDict

    groups = OrderedDict()
    for r in results:
        key = (r["clients"], r["event_size"], r["events_per_client"], r["stories"])
        groups.setdefault(key, []).append(r)

    averaged = []
    for (clients, event_size, events_per_client, stories), reps in groups.items():
        row = {
            "clients": clients,
            "event_size": event_size,
            "events_per_client": events_per_client,
            "stories": stories,
        }
        for mk in METRIC_KEYS:
            vals = [r[mk] for r in reps if r.get(mk) is not None]
            row[mk] = sum(vals) / len(vals) if vals else None
        averaged.append(row)
    return averaged


def write_csv(results: list, csv_path: str):
    if not results:
        print("WARNING: no results to write.")
        return
    fieldnames = [
        "clients",
        "event_size",
        "events_per_client",
        "stories",
        "total_payload_bytes",
        "e2e_bw_MB_s",
        "record_bw_MB_s",
        "record_throughput_ev_s",
    ]
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for r in results:
            writer.writerow(r)
    print(f"CSV written: {csv_path}")


def plot_bandwidth(results: list, plot_path: str):
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("WARNING: matplotlib not installed — skipping plot.")
        return

    if not results:
        return

    # Group by event_size, plot e2e bandwidth vs #clients
    sizes = sorted(set(r["event_size"] for r in results))
    fig, ax = plt.subplots(figsize=(10, 6))

    for sz in sizes:
        subset = sorted(
            [r for r in results if r["event_size"] == sz],
            key=lambda r: r["clients"],
        )
        clients = [r["clients"] for r in subset]
        bw = [r.get("e2e_bw_MB_s") or 0 for r in subset]
        ax.plot(clients, bw, marker="o", label=f"{sz}B payload")

    ax.set_xlabel("Number of clients (MPI ranks)")
    ax.set_ylabel("End-to-end bandwidth (MB/s)")
    ax.set_title("ChronoKeeper Write Bandwidth Scaling")
    ax.legend()
    ax.grid(True, alpha=0.3)
    ax.set_xticks(sorted(set(r["clients"] for r in results)))

    fig.tight_layout()
    fig.savefig(plot_path, dpi=150)
    plt.close(fig)
    print(f"Plot saved: {plot_path}")


def analyze(log_path: str):
    raw_results = parse_log(log_path)
    if not raw_results:
        sys.exit(f"ERROR: no [RUN] blocks found in {log_path}")

    results = _average_repetitions(raw_results)

    base = Path(log_path).stem  # e.g. perf_20260325_143012
    out_dir = Path(log_path).parent
    csv_path = str(out_dir / f"{base}.csv")
    plot_path = str(out_dir / f"{base}_bandwidth.png")

    write_csv(results, csv_path)
    plot_bandwidth(results, plot_path)

    # Print summary table
    print(
        f"\n{'clients':>8} {'size':>6} {'e2e_bw':>10} {'rec_bw':>10} {'rec_tput':>12}"
    )
    print("-" * 52)
    for r in results:
        print(
            f"{r['clients']:>8} "
            f"{r['event_size']:>6} "
            f"{r.get('e2e_bw_MB_s', 0) or 0:>10.2f} "
            f"{r.get('record_bw_MB_s', 0) or 0:>10.2f} "
            f"{r.get('record_throughput_ev_s', 0) or 0:>12.1f}"
        )


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(
        description="ChronoKeeper performance regression tests",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="mode", required=True)

    # --- run ---
    p_run = sub.add_parser("run", help="Run a sweep and log results")
    p_run.add_argument(
        "--install-dir",
        default=os.path.expanduser("~/chronolog-install"),
        help="ChronoLog install prefix (default: ~/chronolog-install)",
    )
    p_run.add_argument(
        "--clients",
        nargs="+",
        type=int,
        default=[1, 2, 4, 8],
        help="Client counts to sweep (default: 1 2 4 8)",
    )
    p_run.add_argument(
        "--event-sizes",
        nargs="+",
        type=int,
        default=[64, 256, 1024, 4096],
        help="Event payload sizes in bytes (default: 64 256 1024 4096)",
    )
    p_run.add_argument(
        "--stories",
        type=int,
        default=1,
        help="Number of stories per client (default: 1)",
    )
    p_run.add_argument(
        "--keepers",
        type=int,
        default=1,
        help="Number of keeper processes (default: 1)",
    )
    p_run.add_argument(
        "--hostfile",
        default=None,
        help="MPI hostfile for multi-node runs (passed to mpiexec -hostfile). "
        "Target data volume scales with the number of nodes.",
    )
    p_run.add_argument(
        "--reps",
        type=int,
        default=1,
        help="Repetitions per configuration for averaging (default: 1)",
    )
    p_run.add_argument(
        "--protocol",
        choices=["ofi+sockets", "ofi+verbs"],
        default="ofi+sockets",
        help="Transport protocol for data-path services (default: ofi+sockets)",
    )
    p_run.add_argument(
        "--analyze",
        action="store_true",
        help="Run analyze after the sweep completes",
    )

    # --- analyze ---
    p_analyze = sub.add_parser("analyze", help="Analyze a previous log file")
    p_analyze.add_argument(
        "--log",
        required=True,
        help="Path to a perf_*.log file from a previous run",
    )

    args = parser.parse_args()

    if args.mode == "run":
        log_path = run_sweep(args)
        if args.analyze:
            analyze(log_path)
    elif args.mode == "analyze":
        analyze(args.log)


if __name__ == "__main__":
    main()
