#!/usr/bin/env python3

import argparse
import json
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
    parser = argparse.ArgumentParser(description="Run a small ChronoLog range-retrieval benchmark.")
    parser.add_argument("--config", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--operation-count", type=int, default=10)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--node-count", type=int, default=2)
    parser.add_argument("--client-count", type=int, default=1)
    parser.add_argument("--flush-wait-seconds", type=float, default=130.0)
    args = parser.parse_args()

    try:
        import py_chronolog_client
    except ImportError as exc:
        raise SystemExit(f"failed to import py_chronolog_client: {exc}")

    config = json.loads(Path(args.config).read_text())
    portal = config["chrono_client"]["VisorClientPortalService"]["rpc"]
    query = config["chrono_client"]["ClientQueryService"]["rpc"]

    client_conf = py_chronolog_client.ClientPortalServiceConf(
        portal["protocol_conf"],
        portal["service_ip"],
        int(portal["service_base_port"]),
        int(portal["service_provider_id"]),
    )
    query_conf = py_chronolog_client.ClientQueryServiceConf(
        query["protocol_conf"],
        query["service_ip"],
        int(query["service_base_port"]),
        int(query["service_provider_id"]),
    )
    client = py_chronolog_client.Client(client_conf, query_conf)

    result_root = Path(args.result_dir)
    chronolog_dir = result_root / "chronolog"
    chronolog_dir.mkdir(parents=True, exist_ok=True)

    suffix = f"{int(time.time() * 1000)}"
    chronicle = f"phase0_range_chronicle_{suffix}"
    story = f"phase0_range_story_{suffix}"
    payload = "r" * args.message_size_bytes
    attrs = {}
    flags = 1
    replay_latencies_ms = []
    retrieved = 0

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

        for index in range(args.operation_count):
            handle.log_event(f"{index}:{payload}")

        if args.flush_wait_seconds > 0:
            time.sleep(args.flush_wait_seconds)

        events = py_chronolog_client.EventList()
        started = time.perf_counter()
        ret = client.ReplayStory(chronicle, story, 1, 2000000000000000000, events)
        elapsed = time.perf_counter() - started
        replay_latencies_ms.append(elapsed * 1000)
        if ret != 0:
            raise RuntimeError(f"ReplayStory returned {ret}")
        retrieved = len(events)
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

    duration = replay_latencies_ms[0] / 1000 if replay_latencies_ms else 0.0
    throughput = retrieved / duration if duration > 0 else 0.0
    metrics = {
        "system": "chronolog",
        "workflow": "range_retrieval",
        "node_count": args.node_count,
        "client_count": args.client_count,
        "message_size_bytes": args.message_size_bytes,
        "operation_count": args.operation_count,
        "duration_seconds": duration,
        "throughput_ops_per_sec": throughput,
        "avg_latency_ms": statistics.fmean(replay_latencies_ms) if replay_latencies_ms else None,
        "p50_latency_ms": percentile(replay_latencies_ms, 50),
        "p95_latency_ms": percentile(replay_latencies_ms, 95),
        "p99_latency_ms": percentile(replay_latencies_ms, 99),
        "success": retrieved >= args.operation_count,
        "retrieved_event_count": retrieved,
    }
    (chronolog_dir / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    if not metrics["success"]:
        raise SystemExit(1)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"chronolog range retrieval benchmark failed: {exc}", file=sys.stderr)
        raise
