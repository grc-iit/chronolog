#!/usr/bin/env python3

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path


def percentile(values, pct):
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, round((pct / 100) * (len(ordered) - 1))))
    return ordered[index]


def wait_for_oldest(pending_writes):
    if not pending_writes:
        return 0, 0.0
    item = pending_writes.pop(0)
    future = item[0] if isinstance(item, tuple) else item
    started = time.perf_counter()
    timestamp = future.wait()
    return timestamp, time.perf_counter() - started


def pending_future_count(pending_writes):
    count = 0
    for item in pending_writes:
        count += int(item[1]) if isinstance(item, tuple) else 1
    return count


def log_event_future_count(future):
    if not future.valid():
        return 0
    future_count = getattr(future, "future_count", None)
    if future_count is None:
        return 1
    return int(future_count())


def drain_pending(pending_writes, stats):
    last_timestamp = 0
    while pending_writes:
        last_timestamp, wait_elapsed = wait_for_oldest(pending_writes)
        stats["future_wait_count"] += 1
        stats["future_wait_seconds"] += wait_elapsed
        stats["future_wait_max_seconds"] = max(stats["future_wait_max_seconds"], wait_elapsed)
        if last_timestamp == 0:
            return 0
    return last_timestamp


def producer_wait_label(producer_wait_mode, producer_outstanding, producer_batch_size):
    if producer_wait_mode == "after_loop":
        return (
            "after_loop_wait"
            if producer_batch_size <= 1
            else f"after_loop_wait_batch_{producer_batch_size}_records"
        )
    if producer_wait_mode == "per_event":
        return "per_event_wait" if producer_batch_size <= 1 else f"batch_wait_after_{producer_batch_size}_records"
    if producer_wait_mode == "bounded_api":
        return f"bounded_api_wait_after_{producer_outstanding}_calls_batch_{producer_batch_size}"
    if producer_wait_mode == "bounded_per_keeper":
        return f"bounded_per_keeper_wait_after_{producer_outstanding}_keeper_futures_batch_{producer_batch_size}"
    if producer_wait_mode == "bounded_futures":
        return f"bounded_futures_wait_after_{producer_outstanding}_keeper_futures_batch_{producer_batch_size}"
    return f"bounded_outstanding_wait_after_{producer_outstanding}_calls_batch_{producer_batch_size}"


def effective_wait_mode(producer_wait_mode, producer_outstanding):
    if producer_wait_mode != "auto":
        return producer_wait_mode
    return "per_event" if producer_outstanding <= 1 else "bounded_outstanding"


def new_client_append_stats(client_index):
    return {
        "client_index": client_index,
        "submit_call_count": 0,
        "submit_event_count": 0,
        "submit_payload_bytes": 0,
        "submit_seconds": 0.0,
        "submit_max_seconds": 0.0,
        "future_count": 0,
        "future_count_max": 0,
        "future_wait_count": 0,
        "future_wait_seconds": 0.0,
        "future_wait_max_seconds": 0.0,
    }


def submit_payloads(handle, payloads, producer_wait_mode, producer_outstanding, pending_writes, stats):
    if not payloads:
        return 0
    wait_mode = effective_wait_mode(producer_wait_mode, producer_outstanding)
    submit_started = time.perf_counter()
    if len(payloads) == 1:
        if wait_mode == "per_event":
            timestamp = handle.log_event(payloads[0])
            submit_elapsed = time.perf_counter() - submit_started
            stats["submit_call_count"] += 1
            stats["submit_event_count"] += 1
            stats["submit_payload_bytes"] += len(payloads[0])
            stats["submit_seconds"] += submit_elapsed
            stats["submit_max_seconds"] = max(stats["submit_max_seconds"], submit_elapsed)
            stats["future_count"] += 1
            stats["future_count_max"] = max(stats["future_count_max"], 1)
            return timestamp
        future = handle.log_event_async(payloads[0])
    else:
        future = handle.log_events_async(payloads)
    submit_elapsed = time.perf_counter() - submit_started
    future_count = log_event_future_count(future)
    stats["submit_call_count"] += 1
    stats["submit_event_count"] += len(payloads)
    stats["submit_payload_bytes"] += sum(len(payload) for payload in payloads)
    stats["submit_seconds"] += submit_elapsed
    stats["submit_max_seconds"] = max(stats["submit_max_seconds"], submit_elapsed)
    stats["future_count"] += future_count
    stats["future_count_max"] = max(stats["future_count_max"], future_count)
    if wait_mode == "per_event":
        wait_started = time.perf_counter()
        timestamp = future.wait()
        wait_elapsed = time.perf_counter() - wait_started
        stats["future_wait_count"] += 1
        stats["future_wait_seconds"] += wait_elapsed
        stats["future_wait_max_seconds"] = max(stats["future_wait_max_seconds"], wait_elapsed)
        return timestamp
    if not future.valid():
        return 0
    pending_writes.append((future, future_count))
    if wait_mode == "bounded_outstanding" and len(pending_writes) >= producer_outstanding:
        timestamp, wait_elapsed = wait_for_oldest(pending_writes)
        stats["future_wait_count"] += 1
        stats["future_wait_seconds"] += wait_elapsed
        stats["future_wait_max_seconds"] = max(stats["future_wait_max_seconds"], wait_elapsed)
        return timestamp
    if wait_mode == "bounded_futures" and pending_future_count(pending_writes) >= producer_outstanding:
        timestamp, wait_elapsed = wait_for_oldest(pending_writes)
        stats["future_wait_count"] += 1
        stats["future_wait_seconds"] += wait_elapsed
        stats["future_wait_max_seconds"] = max(stats["future_wait_max_seconds"], wait_elapsed)
        return timestamp
    return 1


def append_client_records(record, operation_count, payload, producer_batch_size, producer_wait_mode, producer_outstanding):
    handle = record["handle"]
    client_index = record["client_index"]
    stats = new_client_append_stats(client_index)
    wait_mode = effective_wait_mode(producer_wait_mode, producer_outstanding)
    if wait_mode == "bounded_api":
        import py_chronolog_client

        appender = py_chronolog_client.BoundedLogEventAppender(
            handle,
            producer_batch_size,
            producer_outstanding,
        )
        pending_payloads = []
        for index in range(operation_count):
            pending_payloads.append(f"{client_index}:{index}:{payload}")
            if len(pending_payloads) < producer_batch_size:
                continue
            submit_started = time.perf_counter()
            timestamp = appender.append_many(pending_payloads)
            submit_elapsed = time.perf_counter() - submit_started
            stats["submit_call_count"] += 1
            stats["submit_event_count"] += len(pending_payloads)
            stats["submit_payload_bytes"] += sum(len(item) for item in pending_payloads)
            stats["submit_seconds"] += submit_elapsed
            stats["submit_max_seconds"] = max(stats["submit_max_seconds"], submit_elapsed)
            if timestamp == 0:
                raise RuntimeError(f"BoundedLogEventAppender.append_many failed at client {client_index} index {index}")
            pending_payloads = []
        if pending_payloads:
            submit_started = time.perf_counter()
            timestamp = appender.append_many(pending_payloads)
            submit_elapsed = time.perf_counter() - submit_started
            stats["submit_call_count"] += 1
            stats["submit_event_count"] += len(pending_payloads)
            stats["submit_payload_bytes"] += sum(len(item) for item in pending_payloads)
            stats["submit_seconds"] += submit_elapsed
            stats["submit_max_seconds"] = max(stats["submit_max_seconds"], submit_elapsed)
            if timestamp == 0:
                raise RuntimeError(f"BoundedLogEventAppender.append_many failed while flushing client {client_index} batch")
        wait_started = time.perf_counter()
        timestamp = appender.flush()
        wait_elapsed = time.perf_counter() - wait_started
        stats["future_wait_count"] += 1
        stats["future_wait_seconds"] += wait_elapsed
        stats["future_wait_max_seconds"] = max(stats["future_wait_max_seconds"], wait_elapsed)
        if timestamp == 0:
            raise RuntimeError(f"BoundedLogEventAppender.flush failed for client {client_index}")
        return stats

    if wait_mode == "bounded_per_keeper":
        appender = handle.make_per_keeper_bounded_appender(
            producer_batch_size,
            producer_outstanding,
        )
        pending_payloads = []
        for index in range(operation_count):
            pending_payloads.append(f"{client_index}:{index}:{payload}")
            if len(pending_payloads) < producer_batch_size:
                continue
            submit_started = time.perf_counter()
            timestamp = appender.append_many(pending_payloads)
            submit_elapsed = time.perf_counter() - submit_started
            stats["submit_call_count"] += 1
            stats["submit_event_count"] += len(pending_payloads)
            stats["submit_payload_bytes"] += sum(len(item) for item in pending_payloads)
            stats["submit_seconds"] += submit_elapsed
            stats["submit_max_seconds"] = max(stats["submit_max_seconds"], submit_elapsed)
            if timestamp == 0:
                raise RuntimeError(
                    f"PerKeeperBoundedLogEventAppender.append_many failed at client {client_index} index {index}"
                )
            pending_payloads = []
        if pending_payloads:
            submit_started = time.perf_counter()
            timestamp = appender.append_many(pending_payloads)
            submit_elapsed = time.perf_counter() - submit_started
            stats["submit_call_count"] += 1
            stats["submit_event_count"] += len(pending_payloads)
            stats["submit_payload_bytes"] += sum(len(item) for item in pending_payloads)
            stats["submit_seconds"] += submit_elapsed
            stats["submit_max_seconds"] = max(stats["submit_max_seconds"], submit_elapsed)
            if timestamp == 0:
                raise RuntimeError(
                    f"PerKeeperBoundedLogEventAppender.append_many failed while flushing client {client_index} batch"
                )
        wait_started = time.perf_counter()
        timestamp = appender.flush()
        wait_elapsed = time.perf_counter() - wait_started
        stats["future_wait_count"] += 1
        stats["future_wait_seconds"] += wait_elapsed
        stats["future_wait_max_seconds"] = max(stats["future_wait_max_seconds"], wait_elapsed)
        if timestamp == 0:
            raise RuntimeError(f"PerKeeperBoundedLogEventAppender.flush failed for client {client_index}")
        stats["future_count"] += int(appender.future_count())
        stats["future_count_max"] = max(
            stats["future_count_max"],
            int(appender.future_count_max_per_call()),
        )
        internal_wait_count = int(appender.future_wait_count())
        if internal_wait_count:
            stats["future_wait_count"] = internal_wait_count
            stats["future_wait_seconds"] = float(appender.future_wait_ns()) / 1000000000.0
            stats["future_wait_max_seconds"] = float(appender.future_wait_max_ns()) / 1000000000.0
        return stats

    pending_writes = []
    pending_payloads = []
    for index in range(operation_count):
        pending_payloads.append(f"{client_index}:{index}:{payload}")
        if len(pending_payloads) < producer_batch_size:
            continue
        timestamp = submit_payloads(
            handle,
            pending_payloads,
            producer_wait_mode,
            producer_outstanding,
            pending_writes,
            stats,
        )
        pending_payloads = []
        if timestamp == 0:
            raise RuntimeError(f"log_event failed at client {client_index} index {index}")
    timestamp = submit_payloads(
        handle,
        pending_payloads,
        producer_wait_mode,
        producer_outstanding,
        pending_writes,
        stats,
    )
    if pending_payloads and timestamp == 0:
        raise RuntimeError(f"log_event failed while flushing client {client_index} batch")
    had_pending_writes = bool(pending_writes)
    timestamp = drain_pending(pending_writes, stats)
    if had_pending_writes and timestamp == 0:
        raise RuntimeError(f"log_event failed while draining client {client_index} pending writes")
    return stats


def run(cmd, *, timeout=None):
    return subprocess.run(cmd, text=True, capture_output=True, timeout=timeout)


def ssh(host, command, *, timeout=None):
    env = os.environ.copy()
    env.pop("LD_LIBRARY_PATH", None)
    return subprocess.run(
        ["ssh", "-n", host, "bash", "-lc", command],
        text=True,
        capture_output=True,
        timeout=timeout,
        env=env,
    )


def self_safe_regex(literal):
    escaped = re.escape(str(literal))
    if not escaped:
        return escaped
    return f"[{escaped[0]}]{escaped[1:]}"


def keeper_metadata(host_config):
    config = json.loads(Path(host_config).read_text())
    keeper = config["chrono_keeper"]
    return {
        "ports": [
            int(keeper["KeeperRecordingService"]["rpc"]["service_base_port"]),
            int(keeper["KeeperDataStoreAdminService"]["rpc"]["service_base_port"]),
        ],
        "monitor_log": keeper.get("Monitoring", {}).get("monitor", {}).get("file", ""),
    }


def read_from_offset(path, offset):
    try:
        path = Path(path)
        size = path.stat().st_size
        if offset > size:
            offset = 0
        with path.open("rb") as stream:
            stream.seek(offset)
            return stream.read().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def restart_keepers(hosts, wrapper, config_dir, log_dir, stop_timeout, start_wait, pre_crash_stats_snapshot_wait):
    restart_records = []
    gperftools_cpu_profile_signal = os.environ.get("CHRONOLOG_GPERFTOOLS_CPU_PROFILE_SIGNAL", "").strip()
    for host in hosts:
        host_config = Path(config_dir) / f"default-chrono-conf.json.1.keeper.{host}"
        restart_log = Path(log_dir) / f"chrono-keeper-{host}.restart.log"
        metadata = keeper_metadata(host_config)
        monitor_log = Path(metadata["monitor_log"]) if metadata["monitor_log"] else Path(log_dir) / f"chrono-keeper-{host}.log"
        try:
            monitor_offset = monitor_log.stat().st_size
        except FileNotFoundError:
            monitor_offset = 0
        keeper_process_regex = self_safe_regex(host_config)
        ports = metadata["ports"]
        port_probe = " ; ".join(
            f"echo PORT {port}; fuser -v {port}/tcp 2>&1 || true; ss -H -ltnp 'sport = :{port}' 2>&1 || true"
            for port in ports
        )
        port_kill = " ; ".join(f"fuser -k -9 {port}/tcp 2>/dev/null || true" for port in ports)
        port_wait = " && ".join(f"! fuser {port}/tcp >/dev/null 2>&1" for port in ports)
        port_pid_probe = " ; ".join(f"fuser {port}/tcp 2>/dev/null || true" for port in ports)
        stats_snapshot_cmd = ""
        if pre_crash_stats_snapshot_wait > 0:
            snapshot_request_file = shlex.quote(str(monitor_log) + ".snapshot_request")
            profile_stop_cmd = ""
            if gperftools_cpu_profile_signal:
                profile_stop_cmd = (
                    f"if [[ -n \"${{snapshot_pids}}\" ]]; then "
                    f"kill -{shlex.quote(gperftools_cpu_profile_signal)} ${{snapshot_pids}} 2>/dev/null || true; "
                    f"fi; "
                )
            stats_snapshot_cmd = (
                f"echo REQUEST_STATS_SNAPSHOT; "
                f"touch {snapshot_request_file} 2>/dev/null || true; "
                f"snapshot_pids=\"$( (pgrep -f -- {keeper_process_regex!r} 2>/dev/null || true; "
                f"{port_pid_probe}) | tr ' ' '\\n' | awk '/^[0-9]+$/ {{print}}' | sort -u | tr '\\n' ' ' )\"; "
                f"echo SNAPSHOT_PIDS=${{snapshot_pids}}; "
                f"if [[ -n \"${{snapshot_pids}}\" ]]; then kill -USR1 ${{snapshot_pids}} 2>/dev/null || true; fi; "
                f"sleep {pre_crash_stats_snapshot_wait}; "
                f"{profile_stop_cmd}"
            )
        stop_cmd = (
            f"echo BEFORE_STOP; {port_probe}; "
            f"{stats_snapshot_cmd}"
            f"pkill --signal 9 -f -- {keeper_process_regex!r} 2>/dev/null || true; "
            f"{port_kill}; "
            f"deadline=$((SECONDS+{stop_timeout})); "
            f"while pgrep -f -- {keeper_process_regex!r} >/dev/null 2>&1 || ! ( {port_wait} ); do "
            f"  if [[ $SECONDS -ge $deadline ]]; then break; fi; "
            f"  sleep 1; "
            f"done; "
            f"echo AFTER_STOP; pgrep -f -- {keeper_process_regex!r} || true; {port_probe}"
        )
        restart_records.append(
            {
                "host": host,
                "config": str(host_config),
                "ports": ports,
                "restart_log": str(restart_log),
                "monitor_log": str(monitor_log),
                "monitor_log_offset_before_restart": monitor_offset,
                "keeper_process_regex": keeper_process_regex,
                "port_probe_command": port_probe,
                "stop_command": stop_cmd,
                "pre_crash_stats_snapshot_wait_seconds": pre_crash_stats_snapshot_wait,
                "stop_returncode": None,
                "stop_stdout": "",
                "stop_stderr": "",
                "start_command": "",
                "start_returncode": None,
                "start_stdout": "",
                "start_stderr": "",
                "post_start_returncode": None,
                "post_start_stdout": "",
                "post_start_stderr": "",
            }
        )

    for record in restart_records:
        stop_result = ssh(record["host"], record["stop_command"], timeout=stop_timeout + 10)
        record["stop_returncode"] = stop_result.returncode
        record["stop_stdout"] = stop_result.stdout
        record["stop_stderr"] = stop_result.stderr

    time.sleep(5)

    for record in restart_records:
        restart_log = Path(record["restart_log"])
        restart_log.parent.mkdir(parents=True, exist_ok=True)
        start_cmd = (
            f"cd {shlex.quote(str(Path(wrapper).parent))}; "
            f": > {shlex.quote(str(restart_log))}; "
            f"setsid nohup {shlex.quote(str(wrapper))} --config {shlex.quote(record['config'])} "
            f"> {shlex.quote(str(restart_log))} 2>&1 < /dev/null & "
            f"echo STARTED_WRAPPER_PID=$!"
        )
        record["start_command"] = start_cmd
        start_result = ssh(record["host"], start_cmd, timeout=30)
        record["start_returncode"] = start_result.returncode
        record["start_stdout"] = start_result.stdout
        record["start_stderr"] = start_result.stderr
        if start_result.returncode != 0:
            raise RuntimeError(f"Keeper restart failed on {record['host']}")

    time.sleep(start_wait)

    for record in restart_records:
        post_start_cmd = (
            f"echo PROCESS_PROBE; pgrep -af -- {record['keeper_process_regex']!r} || true; "
            f"echo PORT_PROBE; {record['port_probe_command']}"
        )
        post_result = ssh(record["host"], post_start_cmd, timeout=30)
        record["post_start_returncode"] = post_result.returncode
        record["post_start_stdout"] = post_result.stdout
        record["post_start_stderr"] = post_result.stderr
        restart_log = Path(record["restart_log"])
        try:
            restart_text = restart_log.read_text(errors="replace")
        except FileNotFoundError:
            restart_text = ""
        monitor_text = read_from_offset(record["monitor_log"], record["monitor_log_offset_before_restart"])
        combined_health_text = restart_text + "\n" + monitor_text
        record["restart_log_contains_registration"] = "Successfully registered with ChronoVisor" in restart_text
        record["restart_log_contains_recovery"] = "recovered" in restart_text and "journal tail index records" in restart_text
        record["restart_log_contains_bind_error"] = "Could not initialize hg_class" in restart_text
        record["monitor_log_after_restart_bytes"] = len(monitor_text.encode("utf-8"))
        record["monitor_log_contains_running"] = "Running ChronoKeeper Server" in monitor_text
        record["monitor_log_contains_registration"] = "Successfully registered with ChronoVisor" in monitor_text
        record["monitor_log_contains_recovery"] = "recovered" in monitor_text and "journal tail index records" in monitor_text
        record["monitor_log_contains_bind_error"] = "Could not initialize hg_class" in monitor_text
        record["post_start_process_seen"] = (
            str(record["config"]) in record["post_start_stdout"] or "chrono-keeper" in record["post_start_stdout"]
        )
        record["post_start_ports_seen"] = all(
            f"PORT {port}" in record["post_start_stdout"] and "LISTEN" in record["post_start_stdout"]
            for port in record["ports"]
        )
        record["combined_health_contains_registration"] = "Successfully registered with ChronoVisor" in combined_health_text
        record["combined_health_contains_recovery"] = (
            "recovered" in combined_health_text and "journal tail index records" in combined_health_text
        )
        record["combined_health_contains_bind_error"] = "Could not initialize hg_class" in combined_health_text
    return restart_records


def main():
    parser = argparse.ArgumentParser(description="Validate ChronoLog Keeper restart recovery through client RPCs.")
    parser.add_argument("--config", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--operation-count", type=int, default=1000)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--completion-mode", default="keeper_journal_fdatasync")
    parser.add_argument("--node-count", type=int, default=4)
    parser.add_argument("--client-count", type=int, default=1)
    parser.add_argument("--keeper-hosts", required=True)
    parser.add_argument("--keeper-wrapper", required=True)
    parser.add_argument("--config-dir", required=True)
    parser.add_argument("--log-dir", required=True)
    parser.add_argument("--restart-stop-timeout", type=int, default=60)
    parser.add_argument("--restart-start-wait", type=int, default=20)
    parser.add_argument("--pre-crash-stats-snapshot-wait", type=int, default=2)
    parser.add_argument("--producer-outstanding", type=int, default=1)
    parser.add_argument("--producer-batch-size", type=int, default=1)
    parser.add_argument(
        "--producer-wait-mode",
        choices=(
            "auto",
            "per_event",
            "bounded_outstanding",
            "bounded_futures",
            "bounded_api",
            "bounded_per_keeper",
            "after_loop",
        ),
        default="auto",
        help=(
            "Producer wait policy. auto preserves legacy behavior: per-event when outstanding<=1, "
            "bounded outstanding otherwise. bounded_futures bounds pending Keeper RPC futures. "
            "bounded_api uses StoryHandle.log_events_bounded. "
            "bounded_per_keeper uses a streaming StoryHandle per-Keeper bounded appender. "
            "after_loop submits async writes and waits during final drain."
        ),
    )
    parser.add_argument(
        "--client-execution-mode",
        choices=("sequential", "threads"),
        default="sequential",
        help="How to execute multiple client handles. sequential preserves older rows; threads gives real concurrent client pressure.",
    )
    args = parser.parse_args()
    if args.producer_outstanding < 1:
        raise SystemExit("--producer-outstanding must be >= 1")
    if args.producer_batch_size < 1:
        raise SystemExit("--producer-batch-size must be >= 1")
    if args.producer_wait_mode in {"bounded_outstanding", "bounded_api", "bounded_per_keeper"} and args.producer_outstanding < 1:
        raise SystemExit(f"--producer-outstanding must be >= 1 for {args.producer_wait_mode}")

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

    result_root = Path(args.result_dir)
    chronolog_dir = result_root / "chronolog"
    chronolog_dir.mkdir(parents=True, exist_ok=True)
    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)

    hosts = [line.strip() for line in Path(args.keeper_hosts).read_text().splitlines() if line.strip()]
    suffix = f"{int(time.time() * 1000)}"
    chronicle = f"keeper_restart_chronicle_{suffix}"
    story_prefix = f"keeper_restart_story_{suffix}"
    payload = "k" * args.message_size_bytes
    attrs = {}
    flags = 1
    replay_latencies_ms = []
    retrieved_by_story = []
    retrieved = 0
    restart_records = []
    append_elapsed = 0.0
    replay_return_codes = []
    workflow_error = ""
    client_records = []
    client_append_stats = []

    try:
        for client_index in range(args.client_count):
            story = f"{story_prefix}_{client_index}"
            client = py_chronolog_client.Client(client_conf, query_conf)
            ret = client.Connect()
            if ret != 0:
                raise RuntimeError(f"Connect client {client_index} returned {ret}")
            if client_index == 0:
                ret = client.CreateChronicle(chronicle, attrs, flags)
                if ret != 0:
                    raise RuntimeError(f"CreateChronicle returned {ret}")
            ret, handle = client.AcquireStory(chronicle, story, attrs, flags)
            if ret != 0 or handle is None:
                raise RuntimeError(f"AcquireStory client {client_index} returned {ret}")
            client_records.append(
                {
                    "client_index": client_index,
                    "client": client,
                    "story": story,
                    "handle": handle,
                }
            )

        append_started = time.perf_counter()
        if args.client_execution_mode == "threads":
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.client_count) as executor:
                futures = [
                    executor.submit(
                        append_client_records,
                        record,
                        args.operation_count,
                        payload,
                        args.producer_batch_size,
                        args.producer_wait_mode,
                        args.producer_outstanding,
                    )
                    for record in client_records
                ]
                for future in concurrent.futures.as_completed(futures):
                    client_append_stats.append(future.result())
        else:
            for record in client_records:
                client_append_stats.append(
                    append_client_records(
                        record,
                        args.operation_count,
                        payload,
                        args.producer_batch_size,
                        args.producer_wait_mode,
                        args.producer_outstanding,
                    )
                )
        append_elapsed = time.perf_counter() - append_started

        restart_records = restart_keepers(
            hosts,
            Path(args.keeper_wrapper),
            Path(args.config_dir),
            log_dir,
            args.restart_stop_timeout,
            args.restart_start_wait,
            args.pre_crash_stats_snapshot_wait,
        )

        started = time.perf_counter()
        for record in client_records:
            events = py_chronolog_client.EventList()
            replay_started = time.perf_counter()
            ret = record["client"].ReplayStory(chronicle, record["story"], 1, 2000000000000000000, events)
            elapsed = time.perf_counter() - replay_started
            replay_return_codes.append(ret)
            replay_latencies_ms.append(elapsed * 1000)
            story_retrieved = len(events)
            retrieved_by_story.append(
                {
                    "client_index": record["client_index"],
                    "story": record["story"],
                    "retrieved_event_count": story_retrieved,
                    "expected_retrieved_event_count": args.operation_count,
                    "replay_return_code": ret,
                }
            )
            retrieved += story_retrieved
            if ret != 0:
                raise RuntimeError(f"ReplayStory client {record['client_index']} returned {ret}")
        elapsed = time.perf_counter() - started
    except Exception as exc:
        workflow_error = str(exc)
    finally:
        for record in client_records:
            client = record["client"]
            story = record["story"]
            try:
                client.ReleaseStory(chronicle, story)
            except Exception:
                pass
            try:
                client.DestroyStory(chronicle, story)
            except Exception:
                pass
        if client_records:
            try:
                client_records[0]["client"].DestroyChronicle(chronicle)
            except Exception:
                pass
        for record in client_records:
            try:
                record["client"].Disconnect()
            except Exception:
                pass

    expected_total = args.operation_count * args.client_count
    replay_duration = sum(replay_latencies_ms) / 1000 if replay_latencies_ms else 0.0
    append_throughput = expected_total / append_elapsed if append_elapsed > 0 else 0.0
    replay_throughput = retrieved / replay_duration if replay_duration > 0 else 0.0
    restart_command_success = bool(restart_records) and all(
        record["start_returncode"] == 0 for record in restart_records
    )
    restart_health_success = restart_command_success and all(
        record.get("monitor_log_contains_running")
        and record.get("monitor_log_contains_registration")
        and record.get("post_start_process_seen")
        and record.get("post_start_ports_seen")
        and not record.get("combined_health_contains_bind_error")
        for record in restart_records
    )
    no_workflow_error = workflow_error == ""
    retrieved_complete = retrieved >= expected_total
    replay_complete = bool(replay_return_codes) and all(code == 0 for code in replay_return_codes)
    workflow_success = bool(no_workflow_error and retrieved_complete and replay_complete and restart_health_success)
    client_append_stats = [stats for stats in client_append_stats if stats]
    client_submit_call_count = sum(int(stats.get("submit_call_count", 0)) for stats in client_append_stats)
    client_submit_event_count = sum(int(stats.get("submit_event_count", 0)) for stats in client_append_stats)
    client_submit_payload_bytes = sum(int(stats.get("submit_payload_bytes", 0)) for stats in client_append_stats)
    client_submit_seconds = sum(float(stats.get("submit_seconds", 0.0)) for stats in client_append_stats)
    client_submit_max_seconds = max(
        (float(stats.get("submit_max_seconds", 0.0)) for stats in client_append_stats), default=0.0
    )
    client_future_count = sum(int(stats.get("future_count", 0)) for stats in client_append_stats)
    client_future_count_max = max((int(stats.get("future_count_max", 0)) for stats in client_append_stats), default=0)
    client_future_wait_count = sum(int(stats.get("future_wait_count", 0)) for stats in client_append_stats)
    client_future_wait_seconds = sum(float(stats.get("future_wait_seconds", 0.0)) for stats in client_append_stats)
    client_future_wait_max_seconds = max(
        (float(stats.get("future_wait_max_seconds", 0.0)) for stats in client_append_stats), default=0.0
    )
    if args.completion_mode == "keeper_journal_group_fdatasync":
        semantic_boundary = "keeper_restart_recovery_periodic_fdatasync_journal_tail"
        append_ack_boundary = "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync"
        durability_boundary = "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart"
        semantic_notes = (
            "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync batching, "
            "restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes. "
            "Append return is not per-record fsync wait for non-boundary records; durability evidence is the restart/readback gate."
        )
    elif args.completion_mode == "keeper_journal_group_fdatasync_wal_drain":
        semantic_boundary = "keeper_restart_recovery_periodic_fdatasync_wal_drain_journal_tail"
        append_ack_boundary = "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync_and_wal_cursor_enqueue"
        durability_boundary = "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart"
        semantic_notes = (
            "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync batching and WAL-cursor "
            "async drain semantics, restarts Keepers, then reads the same story through Keeper tail RPCs backed by "
            "recovered local journal indexes. Append return is not per-record fsync wait for non-boundary records; "
            "durability evidence is the restart/readback gate."
        )
    elif args.completion_mode == "keeper_journal_group_fdatasync_tail_only":
        semantic_boundary = "keeper_restart_recovery_periodic_fdatasync_tail_only_journal_tail"
        append_ack_boundary = "StoryHandle.log_event_return_after_keeper_journal_write_periodic_fsync_no_timeline_ingest"
        durability_boundary = "keeper_local_journal_group_fdatasync_recovered_after_keeper_restart"
        semantic_notes = (
            "ChronoLog writes through normal client RPCs with periodic Keeper-local journal fdatasync tail-only semantics, "
            "skips the legacy timeline/archive consumer, restarts Keepers, then reads the same story through Keeper tail "
            "RPCs backed by recovered local journal indexes. Append return is not per-record fsync wait for non-boundary "
            "records; durability evidence is the restart/readback gate. This is not archive/storage readback evidence."
        )
    elif args.completion_mode == "keeper_journal_group_commit_tail_only":
        semantic_boundary = "keeper_restart_recovery_group_commit_tail_only_journal_tail"
        append_ack_boundary = "StoryHandle.log_event_return_after_keeper_journal_group_commit_fdatasync_no_timeline_ingest"
        durability_boundary = "keeper_local_journal_group_commit_fdatasync_recovered_after_keeper_restart"
        semantic_notes = (
            "ChronoLog writes through normal client RPCs to a single Keeper journal owner per shard. The owner writes "
            "a batch, fdatasyncs once, then completes all appends in that batch; legacy timeline/archive ingestion is "
            "skipped. Keepers are restarted and the same story is read through recovered Keeper journal indexes. This "
            "is not archive/storage readback evidence."
        )
    elif args.completion_mode == "keeper_journal_group_commit_deferred_tail_only":
        semantic_boundary = "keeper_restart_recovery_group_commit_deferred_rpc_tail_only_journal_tail"
        append_ack_boundary = "StoryHandle.log_event_return_after_deferred_keeper_journal_group_commit_fdatasync_no_timeline_ingest"
        durability_boundary = "keeper_local_journal_group_commit_fdatasync_recovered_after_keeper_restart"
        semantic_notes = (
            "ChronoLog writes through normal client RPCs to a single Keeper journal owner per shard. The owner writes "
            "a batch, fdatasyncs once, then completes deferred RPC responses for all appends in that batch; legacy "
            "timeline/archive ingestion is skipped. Keepers are restarted and the same story is read through recovered "
            "Keeper journal indexes. This is not archive/storage readback evidence."
        )
    elif args.completion_mode == "keeper_journal_fdatasync_tail_only":
        semantic_boundary = "keeper_restart_recovery_fdatasync_tail_only_journal_tail"
        append_ack_boundary = "StoryHandle.log_event_return_after_keeper_journal_fdatasync_no_timeline_ingest"
        durability_boundary = "keeper_local_journal_fdatasync_recovered_after_keeper_restart"
        semantic_notes = (
            "ChronoLog writes through normal client RPCs with strict Keeper-local journal fdatasync tail-only semantics, "
            "skips legacy timeline/archive ingestion, restarts Keepers, then reads the same story through Keeper tail RPCs "
            "backed by recovered local journal indexes. This is not archive/storage readback evidence."
        )
    else:
        semantic_boundary = "keeper_restart_recovery_fdatasync_journal_tail"
        append_ack_boundary = "StoryHandle.log_event_return_after_keeper_journal_fdatasync"
        durability_boundary = "keeper_local_journal_fdatasync_recovered_after_keeper_restart"
        semantic_notes = (
            "ChronoLog writes through normal client RPCs with strict Keeper-local journal fdatasync semantics, "
            "restarts Keepers, then reads the same story through Keeper tail RPCs backed by recovered local journal indexes."
        )
    durable_complete_before_publish = os.environ.get(
        "CHRONOLOG_KEEPER_JOURNAL_DURABLE_COMPLETE_BEFORE_PUBLISH", "0"
    ) in {"1", "true", "TRUE", "yes", "YES", "on", "ON"}
    if args.completion_mode == "keeper_journal_group_commit_deferred_tail_only" and durable_complete_before_publish:
        semantic_boundary = (
            "keeper_restart_recovery_group_commit_deferred_rpc_durable_before_tail_publish_journal_tail"
        )
        append_ack_boundary = (
            "StoryHandle.log_event_return_after_deferred_keeper_journal_group_commit_fdatasync_before_tail_publish"
        )
        semantic_notes = (
            "ChronoLog writes through normal client RPCs to a single Keeper journal owner per shard. The owner writes "
            "a batch, fdatasyncs once, then completes deferred RPC responses before publishing Keeper tail-index entries; "
            "legacy timeline/archive ingestion is skipped. Keepers are restarted and the same story is read through "
            "recovered Keeper journal indexes. This is durable Keeper-journal evidence, but append return does not imply "
            "that the live tail index was already published, and it is not archive/storage readback evidence."
        )
    metrics = {
        "system": "chronolog",
        "workflow": "keeper_restart_recovery",
        "node_count": args.node_count,
        "nodes": args.node_count,
        "client_count": args.client_count,
        "parallel_clients": args.client_count,
        "message_size_bytes": args.message_size_bytes,
        "operation_count": args.operation_count,
        "operation_count_per_client": args.operation_count,
        "message_count_per_client": args.operation_count,
        "messages_per_client": args.operation_count,
        "total_operation_count": expected_total,
        "total_message_count": expected_total,
        "total_messages": expected_total,
        "total_payload_bytes": expected_total * args.message_size_bytes,
        "parallel_client_count": args.client_count,
        "chronolog_completion_mode": args.completion_mode,
        "chronolog_producer_outstanding": args.producer_outstanding,
        "chronolog_producer_batch_size": args.producer_batch_size,
        "chronolog_producer_wait_policy": args.producer_wait_mode,
        "chronolog_client_batch_keeper_selection": os.environ.get(
            "CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION", "per_event"
        ),
        "chronolog_client_execution_mode": args.client_execution_mode,
        "chronolog_client_append_stats_source": "python_harness",
        "chronolog_producer_wait_mode": producer_wait_label(
            effective_wait_mode(args.producer_wait_mode, args.producer_outstanding),
            args.producer_outstanding,
            args.producer_batch_size,
        ),
        "semantic_boundary": semantic_boundary,
        "append_ack_boundary": append_ack_boundary,
        "durability_boundary": durability_boundary,
        "read_path": "keeper_tail_rpc_recovered_journal_index",
        "storage_backend": "chronolog_keeper_local_journal",
        "semantic_notes": semantic_notes,
        "duration_seconds": append_elapsed,
        "append_duration_seconds": append_elapsed,
        "append_throughput_ops_per_sec": append_throughput,
        "append_avg_latency_ms": (append_elapsed * 1000.0 / expected_total) if expected_total else None,
        "replay_duration_seconds": replay_duration,
        "replay_throughput_ops_per_sec": replay_throughput,
        "throughput_ops_per_sec": append_throughput,
        "avg_latency_ms": statistics.fmean(replay_latencies_ms) if replay_latencies_ms else None,
        "p50_latency_ms": percentile(replay_latencies_ms, 50),
        "p95_latency_ms": percentile(replay_latencies_ms, 95),
        "p99_latency_ms": percentile(replay_latencies_ms, 99),
        "success": workflow_success,
        "error": workflow_error,
        "no_workflow_error": no_workflow_error,
        "retrieved_event_count_complete": retrieved_complete,
        "retrieved_event_count": retrieved,
        "expected_retrieved_event_count": expected_total,
        "retrieved_event_count_by_story": retrieved_by_story,
        "replay_return_code": 0 if replay_return_codes and all(code == 0 for code in replay_return_codes) else (
            replay_return_codes[-1] if replay_return_codes else None
        ),
        "replay_return_codes": replay_return_codes,
        "live_tail_attempted": bool(replay_return_codes),
        "live_tail_succeeded": replay_complete and retrieved_complete,
        "keeper_restart_attempted": True,
        "keeper_restart_command_success": restart_command_success,
        "keeper_restart_health_success": restart_health_success,
        "keeper_restart_hosts": hosts,
        "keeper_restart_records": restart_records,
        "client_append_stats": {
            "source": "python_harness",
            "per_client": client_append_stats,
        },
        "client_append_client_count_with_stats": len(client_append_stats),
        "client_append_submit_call_count": client_submit_call_count,
        "client_append_submit_event_count": client_submit_event_count,
        "client_append_expected_event_count": expected_total,
        "client_append_submit_event_count_matches_total": client_submit_event_count == expected_total,
        "client_append_submit_payload_bytes": client_submit_payload_bytes,
        "client_append_submit_avg_us": (
            client_submit_seconds * 1000000.0 / client_submit_call_count if client_submit_call_count else 0.0
        ),
        "client_append_submit_max_us": client_submit_max_seconds * 1000000.0,
        "client_append_future_count": client_future_count,
        "client_append_future_count_avg_per_submit": (
            client_future_count / client_submit_call_count if client_submit_call_count else 0.0
        ),
        "client_append_future_count_max_per_submit": client_future_count_max,
        "client_append_future_wait_count": client_future_wait_count,
        "client_append_future_wait_avg_us": (
            client_future_wait_seconds * 1000000.0 / client_future_wait_count if client_future_wait_count else 0.0
        ),
        "client_append_future_wait_max_us": client_future_wait_max_seconds * 1000000.0,
    }
    (chronolog_dir / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    sys.stdout.flush()
    sys.stderr.flush()
    # The pybind client can double-free during Python interpreter teardown after
    # multiple Client instances. The benchmark has already explicitly released,
    # destroyed, disconnected, and written metrics; exit directly so the row
    # reflects the measured recovery gate instead of extension cleanup.
    os._exit(0 if metrics["success"] else 1)


if __name__ == "__main__":
    main()
