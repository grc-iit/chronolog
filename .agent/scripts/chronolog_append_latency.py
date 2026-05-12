#!/usr/bin/env python3

import argparse
import json
import os
import statistics
import sys
import time
from pathlib import Path


def percentile(values, pct):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, round((pct / 100) * (len(ordered) - 1))))
    return ordered[index]


def main():
    parser = argparse.ArgumentParser(description="Run a small ChronoLog append-latency benchmark.")
    parser.add_argument("--config", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--operation-count", type=int, default=10)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--node-count", type=int, default=2)
    parser.add_argument("--client-count", type=int, default=1)
    parser.add_argument("--workflow", default="append_latency")
    args = parser.parse_args()

    try:
        import py_chronolog_client
    except ImportError as exc:
        raise SystemExit(f"failed to import py_chronolog_client: {exc}")

    config = json.loads(Path(args.config).read_text())
    portal = config["chrono_client"]["VisorClientPortalService"]["rpc"]

    client_conf = py_chronolog_client.ClientPortalServiceConf(
        portal["protocol_conf"],
        portal["service_ip"],
        int(portal["service_base_port"]),
        int(portal["service_provider_id"]),
    )
    client = py_chronolog_client.Client(client_conf)

    result_root = Path(args.result_dir)
    chronolog_dir = result_root / "chronolog"
    chronolog_dir.mkdir(parents=True, exist_ok=True)
    payload = "x" * args.message_size_bytes
    suffix = f"{int(time.time() * 1000)}_{os.getpid()}"
    chronicle = f"phase0_latency_chronicle_{suffix}"
    story = f"phase0_latency_story_{suffix}"
    attrs = {}
    flags = 1
    latencies_ms = []
    success = False

    started = time.perf_counter()
    try:
        ret = client.Connect()
        if ret != 0:
            raise RuntimeError(f"Connect returned {ret}")
        ret = client.CreateChronicle(chronicle, attrs, flags)
        if ret != 0:
            raise RuntimeError(f"CreateChronicle returned {ret}")
        ret, handle = client.AcquireStory(chronicle, story, attrs, flags)
        if ret != 0 or handle is None:
            raise RuntimeError(f"AcquireStory returned {ret}")

        for _ in range(args.operation_count):
            op_started = time.perf_counter()
            handle.log_event(payload)
            latencies_ms.append((time.perf_counter() - op_started) * 1000)

        success = True
    finally:
        try:
            client.ReleaseStory(chronicle, story)
        except Exception:
            pass
        try:
            client.DestroyStory(chronicle, story)
        except Exception:
            pass
        try:
            client.DestroyChronicle(chronicle)
        except Exception:
            pass
        try:
            client.Disconnect()
        except Exception:
            pass

    duration = time.perf_counter() - started
    throughput = args.operation_count / duration if duration > 0 else 0.0
    metrics = {
        "system": "chronolog",
        "workflow": args.workflow,
        "node_count": args.node_count,
        "client_count": args.client_count,
        "message_size_bytes": args.message_size_bytes,
        "operation_count": args.operation_count,
        "duration_seconds": duration,
        "throughput_ops_per_sec": throughput,
        "avg_latency_ms": statistics.fmean(latencies_ms) if latencies_ms else None,
        "p50_latency_ms": percentile(latencies_ms, 50),
        "p95_latency_ms": percentile(latencies_ms, 95),
        "p99_latency_ms": percentile(latencies_ms, 99),
        "success": success,
    }
    metrics_path = chronolog_dir / "metrics.json"
    metrics_path.write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"chronolog append latency benchmark failed: {exc}", file=sys.stderr)
        raise
