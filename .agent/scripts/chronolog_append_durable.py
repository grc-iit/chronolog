#!/usr/bin/env python3

import argparse
import datetime
import json
import multiprocessing
import os
import re
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


def grapher_archived_event_count(log_dir, chronicle, story):
    pattern = re.compile(
        rf"HDF5FileChunkExtractor.*StoryChunk written to file:.*"
        rf"{re.escape(chronicle)}-{re.escape(story)} .* eventCount (\d+)"
    )
    total = 0
    for path in sorted(log_dir.glob("chrono-grapher-*.log*")):
        try:
            text = path.read_text(errors="ignore")
        except OSError:
            continue
        for match in pattern.finditer(text):
            total += int(match.group(1))
    return total


def hdf5_archived_event_count(files):
    files = [Path(path) for path in files]
    try:
        import h5py
    except Exception:
        return external_hdf5_archived_event_count(files)
    total = 0
    saw_readable_file = False
    for path in files:
        try:
            with h5py.File(path, "r") as handle:
                dataset = handle.get("story_chunks/data.vlen_bytes")
                if dataset is None:
                    dataset = handle.get("story_chunks/data.meta")
                if dataset is None:
                    dataset = handle.get("story_chunks/data.raw_meta")
                if dataset is None:
                    dataset = handle.get("story_chunks/data.fixed_meta")
                if dataset is None:
                    continue
                total += int(dataset.shape[0])
                saw_readable_file = True
        except Exception:
            continue
    if not saw_readable_file:
        return None
    return total


def manifest_archived_event_count(files):
    files = [Path(path) for path in files]
    if not files:
        return None
    total = 0
    saw_manifest = False
    for path in files:
        manifest_path = Path(str(path) + ".manifest")
        try:
            data = json.loads(manifest_path.read_text())
        except Exception:
            return None
        try:
            total += int(data["event_count"])
        except (KeyError, TypeError, ValueError):
            return None
        saw_manifest = True
    return total if saw_manifest else None


def external_hdf5_archived_event_count(files):
    python_bin = Path("/usr/bin/python3")
    if not python_bin.exists():
        return None
    script = r"""
import json
import sys
try:
    import h5py
except Exception:
    raise SystemExit(3)
total = 0
saw_readable_file = False
for item in sys.argv[1:]:
    try:
        with h5py.File(item, "r") as handle:
            dataset = handle.get("story_chunks/data.vlen_bytes")
            if dataset is None:
                dataset = handle.get("story_chunks/data.meta")
            if dataset is None:
                dataset = handle.get("story_chunks/data.raw_meta")
            if dataset is None:
                dataset = handle.get("story_chunks/data.fixed_meta")
            if dataset is None:
                continue
            total += int(dataset.shape[0])
            saw_readable_file = True
    except Exception:
        continue
if not saw_readable_file:
    raise SystemExit(2)
print(json.dumps({"event_count": total}))
"""
    try:
        proc = subprocess.run(
            [str(python_bin), "-c", script, *[str(path) for path in files]],
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )
    except Exception:
        return None
    if proc.returncode != 0:
        return None
    try:
        return int(json.loads(proc.stdout).get("event_count", 0))
    except Exception:
        return None


def parse_log_timestamp(line):
    match = re.match(r"\[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]", line)
    if not match:
        return None
    try:
        return datetime.datetime.strptime(match.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
    except ValueError:
        return None


def grapher_archive_stage_times(log_dir, chronicle, story):
    story_id = None
    stage_times = {
        "grapher_start_recording": None,
        "grapher_stop_recording": None,
        "grapher_pipeline_scheduled": None,
        "grapher_pipeline_finalized": None,
        "grapher_hdf5_processing": None,
        "grapher_hdf5_written": None,
    }
    story_identity = f"{chronicle}-{story}"
    start_pattern = re.compile(rf"Starting Story Recording: StoryName={re.escape(story)}, StoryID=(\d+)")
    hdf5_pattern = re.compile(rf"HDF5FileChunkExtractor.*{re.escape(story_identity)} .* eventCount (\d+)")
    for path in sorted(log_dir.glob("chrono-grapher-*.log*")):
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for line in lines:
            timestamp = parse_log_timestamp(line)
            if timestamp is None:
                continue
            start_match = start_pattern.search(line)
            if start_match:
                story_id = start_match.group(1)
                stage_times["grapher_start_recording"] = stage_times["grapher_start_recording"] or timestamp
                continue
            if hdf5_pattern.search(line):
                if "processing chunk" in line:
                    stage_times["grapher_hdf5_processing"] = stage_times["grapher_hdf5_processing"] or timestamp
                elif "StoryChunk written to file" in line:
                    stage_times["grapher_hdf5_written"] = stage_times["grapher_hdf5_written"] or timestamp
                continue
            if story_id is None or story_id not in line:
                continue
            if "Stopping Story Recording" in line:
                stage_times["grapher_stop_recording"] = stage_times["grapher_stop_recording"] or timestamp
            elif "Scheduled pipeline to retire" in line:
                stage_times["grapher_pipeline_scheduled"] = stage_times["grapher_pipeline_scheduled"] or timestamp
            elif "Finalized ingestion handle" in line:
                stage_times["grapher_pipeline_finalized"] = stage_times["grapher_pipeline_finalized"] or timestamp
    return stage_times, story_id


def grapher_archive_stage_metrics(log_dir, story_specs, release_returned_at):
    stages = (
        "grapher_start_recording",
        "grapher_stop_recording",
        "grapher_pipeline_scheduled",
        "grapher_pipeline_finalized",
        "grapher_hdf5_processing",
        "grapher_hdf5_written",
    )
    metrics = {"grapher_archive_stage_story_count": len(story_specs)}
    for stage in stages:
        metrics[f"{stage}_count"] = 0
        metrics[f"{stage}_first_epoch_seconds"] = None
        metrics[f"{stage}_last_epoch_seconds"] = None
        metrics[f"release_to_{stage}_first_seconds"] = None
        metrics[f"release_to_{stage}_last_seconds"] = None

    story_to_id = {}
    id_to_story = {}
    story_id_pattern = re.compile(r"StoryID=?(\d+)")
    story_names = {story: chronicle for chronicle, story in story_specs}
    story_identity = {f"{chronicle}-{story}": story for chronicle, story in story_specs}

    def record(stage, timestamp):
        if timestamp is None:
            return
        metrics[f"{stage}_count"] += 1
        first_key = f"{stage}_first_epoch_seconds"
        last_key = f"{stage}_last_epoch_seconds"
        if metrics[first_key] is None or timestamp < metrics[first_key]:
            metrics[first_key] = timestamp
        if metrics[last_key] is None or timestamp > metrics[last_key]:
            metrics[last_key] = timestamp

    for path in sorted(log_dir.glob("chrono-grapher-*.log*")):
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for line in lines:
            timestamp = parse_log_timestamp(line)
            if timestamp is None:
                continue
            if "Starting Story Recording" in line:
                for story in story_names:
                    if f"StoryName={story}" in line:
                        match = story_id_pattern.search(line)
                        if match:
                            story_to_id[story] = match.group(1)
                            id_to_story[match.group(1)] = story
                        record("grapher_start_recording", timestamp)
                        break
                continue

            matched_story = None
            for story_id, story in id_to_story.items():
                if story_id in line:
                    matched_story = story
                    break
            if matched_story is None:
                for identity, story in story_identity.items():
                    if identity in line:
                        matched_story = story
                        break
            if matched_story is None:
                continue

            if "Stopping Story Recording" in line:
                record("grapher_stop_recording", timestamp)
            elif "Scheduled pipeline to retire" in line:
                record("grapher_pipeline_scheduled", timestamp)
            elif "Finalized ingestion handle" in line:
                record("grapher_pipeline_finalized", timestamp)
            elif "HDF5FileChunkExtractor" in line and "processing chunk" in line:
                record("grapher_hdf5_processing", timestamp)
            elif "HDF5FileChunkExtractor" in line and "StoryChunk written to file" in line:
                record("grapher_hdf5_written", timestamp)

    for stage in stages:
        metrics[f"release_to_{stage}_first_seconds"] = elapsed_since(
            release_returned_at, metrics[f"{stage}_first_epoch_seconds"]
        )
        metrics[f"release_to_{stage}_last_seconds"] = elapsed_since(
            release_returned_at, metrics[f"{stage}_last_epoch_seconds"]
        )
    metrics["grapher_archive_stage_story_ids"] = story_to_id
    return metrics


def grapher_hdf5_write_profile_metrics(log_dir):
    fields = (
        "write_us",
        "writer_total_us",
        "prep_us",
        "open_us",
        "dataset_write_us",
        "dataset_write_call_us",
        "dataset_payload_write_call_us",
        "raw_payload_writev_us",
        "raw_payload_close_us",
        "raw_payload_close_wait_us",
        "raw_payload_write_wait_us",
        "raw_payload_async_write",
        "raw_payload_async_close",
        "raw_payload_async_publish",
        "raw_payload_async_publish_threads",
        "close_us",
        "rename_us",
        "publish_rename_us",
        "archive_manifest_write_us",
        "lock_wait_us",
    )
    metrics = {
        "grapher_hdf5_write_profile_count": 0,
        "grapher_hdf5_write_profile_event_count": 0,
        "grapher_hdf5_write_profile_file_size_bytes_total": 0,
        "grapher_hdf5_write_profile_raw_payload_bytes_total": 0,
    }
    values = {field: [] for field in fields}
    async_publish_fields = (
        "close_wait_us",
        "close_us",
        "publish_rename_us",
        "archive_manifest_write_us",
        "total_us",
    )
    async_publish_values = {field: [] for field in async_publish_fields}
    async_publish_count = 0
    async_publish_success_count = 0
    pair_pattern = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
    for path in sorted(log_dir.glob("chrono-grapher-*.log*")):
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for line in lines:
            if "Write subphase profile" in line:
                pairs = dict(pair_pattern.findall(line))
                metrics["grapher_hdf5_write_profile_count"] += 1
                for key, metric_key in (
                    ("eventCount", "grapher_hdf5_write_profile_event_count"),
                    ("fileSize", "grapher_hdf5_write_profile_file_size_bytes_total"),
                    ("raw_payload_bytes", "grapher_hdf5_write_profile_raw_payload_bytes_total"),
                ):
                    try:
                        metrics[metric_key] += int(float(pairs.get(key, 0)))
                    except ValueError:
                        pass
                for field in fields:
                    try:
                        values[field].append(float(pairs[field]))
                    except (KeyError, ValueError):
                        pass
            elif "Async archive publish completed" in line:
                pairs = dict(pair_pattern.findall(line))
                async_publish_count += 1
                try:
                    async_publish_success_count += int(float(pairs.get("ok", 0)))
                except ValueError:
                    pass
                for field in async_publish_fields:
                    try:
                        async_publish_values[field].append(float(pairs[field]))
                    except (KeyError, ValueError):
                        pass

    for field, samples in values.items():
        prefix = f"grapher_hdf5_{field}"
        metrics[f"{prefix}_sum"] = sum(samples)
        metrics[f"{prefix}_avg"] = sum(samples) / len(samples) if samples else 0.0
        metrics[f"{prefix}_max"] = max(samples) if samples else 0.0
    metrics["grapher_async_archive_publish_count"] = async_publish_count
    metrics["grapher_async_archive_publish_success_count"] = async_publish_success_count
    for field, samples in async_publish_values.items():
        prefix = f"grapher_async_archive_publish_{field}"
        metrics[f"{prefix}_sum"] = sum(samples)
        metrics[f"{prefix}_avg"] = sum(samples) / len(samples) if samples else 0.0
        metrics[f"{prefix}_max"] = max(samples) if samples else 0.0
    return metrics


def control_path_profile_metrics(log_dir):
    tag_prefixes = {
        "ReleaseStoryProfile": "visor_release_story",
        "RecordingStopProfile": "registry_recording_stop",
        "KeeperStopRpcProfile": "registry_keeper_stop_rpc",
        "GrapherStopRpcProfile": "registry_grapher_stop_rpc",
        "KeeperStopStoryProfile": "keeper_stop_story",
        "GrapherStopStoryProfile": "grapher_stop_story",
    }
    pair_pattern = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
    values = {tag: {} for tag in tag_prefixes}
    counts = {tag: 0 for tag in tag_prefixes}

    for path in sorted(log_dir.glob("chrono-*.log*")):
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for line in lines:
            matched_tag = None
            for tag in tag_prefixes:
                if f"[{tag}]" in line:
                    matched_tag = tag
                    break
            if matched_tag is None:
                continue
            counts[matched_tag] += 1
            pairs = dict(pair_pattern.findall(line))
            for key, raw_value in pairs.items():
                try:
                    value = float(raw_value.rstrip(","))
                except ValueError:
                    continue
                values[matched_tag].setdefault(key, []).append(value)

    metrics = {}
    for tag, prefix in tag_prefixes.items():
        metrics[f"{prefix}_profile_count"] = counts[tag]
        for key, samples in values[tag].items():
            if not samples:
                continue
            metric_prefix = f"{prefix}_{key}"
            metrics[f"{metric_prefix}_min"] = min(samples)
            metrics[f"{metric_prefix}_max"] = max(samples)
            metrics[f"{metric_prefix}_avg"] = statistics.fmean(samples)
            metrics[f"{metric_prefix}_sum"] = sum(samples)
    return metrics


def wait_for_control_path_profiles(log_dir, expected_story_count, timeout_seconds=2.0, poll_interval_seconds=0.1):
    deadline = time.perf_counter() + timeout_seconds
    latest = control_path_profile_metrics(log_dir)
    required = (
        "visor_release_story_profile_count",
        "registry_recording_stop_profile_count",
        "registry_keeper_stop_rpc_profile_count",
        "registry_grapher_stop_rpc_profile_count",
        "keeper_stop_story_profile_count",
        "grapher_stop_story_profile_count",
    )
    while time.perf_counter() < deadline:
        if all(latest.get(key, 0) >= expected_story_count for key in required):
            return latest
        time.sleep(poll_interval_seconds)
        latest = control_path_profile_metrics(log_dir)
    return latest


def keeper_to_grapher_drain_metrics(log_dir, story_id, release_returned_at):
    metrics = {
        "keeper_to_grapher_drain_chunk_count": 0,
        "keeper_to_grapher_drain_first_epoch_seconds": None,
        "keeper_to_grapher_drain_last_epoch_seconds": None,
        "release_to_keeper_to_grapher_drain_first_seconds": None,
        "release_to_keeper_to_grapher_drain_last_seconds": None,
    }
    if story_id is None:
        return metrics
    drain_pattern = re.compile(
        rf"Successfully drained a story chunk to Grapher, StoryID: {re.escape(str(story_id))}, StartTime:"
    )
    for path in sorted(log_dir.glob("chrono-keeper-*.log*")):
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for line in lines:
            if not drain_pattern.search(line):
                continue
            timestamp = parse_log_timestamp(line)
            if timestamp is None:
                continue
            metrics["keeper_to_grapher_drain_chunk_count"] += 1
            if metrics["keeper_to_grapher_drain_first_epoch_seconds"] is None:
                metrics["keeper_to_grapher_drain_first_epoch_seconds"] = timestamp
            metrics["keeper_to_grapher_drain_last_epoch_seconds"] = timestamp
    metrics["release_to_keeper_to_grapher_drain_first_seconds"] = elapsed_since(
        release_returned_at, metrics["keeper_to_grapher_drain_first_epoch_seconds"]
    )
    metrics["release_to_keeper_to_grapher_drain_last_seconds"] = elapsed_since(
        release_returned_at, metrics["keeper_to_grapher_drain_last_epoch_seconds"]
    )
    return metrics


def grapher_orphan_chunk_metrics(log_dir, story_id, release_returned_at):
    metrics = {
        "grapher_orphan_chunk_count": 0,
        "grapher_orphan_first_epoch_seconds": None,
        "grapher_orphan_last_epoch_seconds": None,
        "release_to_grapher_orphan_first_seconds": None,
        "release_to_grapher_orphan_last_seconds": None,
    }
    if story_id is None:
        return metrics
    orphan_pattern = re.compile(rf"Orphan chunk for story {re.escape(str(story_id))}\.")
    for path in sorted(log_dir.glob("chrono-grapher-*.log*")):
        try:
            lines = path.read_text(errors="ignore").splitlines()
        except OSError:
            continue
        for line in lines:
            if not orphan_pattern.search(line):
                continue
            timestamp = parse_log_timestamp(line)
            if timestamp is None:
                continue
            metrics["grapher_orphan_chunk_count"] += 1
            if metrics["grapher_orphan_first_epoch_seconds"] is None:
                metrics["grapher_orphan_first_epoch_seconds"] = timestamp
            metrics["grapher_orphan_last_epoch_seconds"] = timestamp
    metrics["release_to_grapher_orphan_first_seconds"] = elapsed_since(
        release_returned_at, metrics["grapher_orphan_first_epoch_seconds"]
    )
    metrics["release_to_grapher_orphan_last_seconds"] = elapsed_since(
        release_returned_at, metrics["grapher_orphan_last_epoch_seconds"]
    )
    return metrics


def elapsed_since(reference, timestamp):
    if reference is None or timestamp is None:
        return None
    return timestamp - reference


def archive_file_stat_metrics(files, release_returned_at, detect_epoch):
    if not files:
        return {
            "archive_file_mtime_epoch_seconds": None,
            "release_to_archive_file_mtime_seconds": None,
            "archive_file_mtime_to_detect_seconds": None,
            "archive_file_size_bytes": 0,
            "archive_sidecar_file_size_bytes": 0,
            "archive_total_file_size_bytes": 0,
        }
    first_file = files[0]
    try:
        stat = first_file.stat()
    except OSError:
        return {
            "archive_file_mtime_epoch_seconds": None,
            "release_to_archive_file_mtime_seconds": None,
            "archive_file_mtime_to_detect_seconds": None,
            "archive_file_size_bytes": 0,
            "archive_sidecar_file_size_bytes": 0,
            "archive_total_file_size_bytes": 0,
        }
    mtime = stat.st_mtime_ns / 1_000_000_000.0
    sidecar_size = 0
    sidecar_path = Path(str(first_file) + ".payload")
    try:
        sidecar_stat = sidecar_path.stat()
        sidecar_size = sidecar_stat.st_size
        mtime = max(mtime, sidecar_stat.st_mtime_ns / 1_000_000_000.0)
    except OSError:
        pass
    return {
        "archive_file_mtime_epoch_seconds": mtime,
        "release_to_archive_file_mtime_seconds": elapsed_since(release_returned_at, mtime),
        "archive_file_mtime_to_detect_seconds": elapsed_since(mtime, detect_epoch),
        "archive_file_size_bytes": stat.st_size,
        "archive_sidecar_file_size_bytes": sidecar_size,
        "archive_total_file_size_bytes": stat.st_size + sidecar_size,
    }


def log_settle_seconds():
    try:
        return max(0.0, float(os.environ.get("CHRONOLOG_ARCHIVE_LOG_SETTLE_SECONDS", "0.5")))
    except ValueError:
        return 0.5


def wait_for_archive_file(output_dir, chronicle, story, timeout_seconds):
    deadline = time.monotonic() + timeout_seconds
    last_files = []
    while time.monotonic() < deadline:
        files = sorted(output_dir.glob(f"{chronicle}.{story}*.h5"))
        if files:
            return files
        last_files = files
        time.sleep(0.1)
    raise RuntimeError(
        f"Timed out waiting for archived story file in {output_dir}; "
        f"files={[str(path) for path in last_files]}"
    )


def wait_for_archive_event_count(
    output_dir,
    chronicle,
    story,
    expected_count,
    timeout_seconds,
    poll_interval_seconds=1.0,
    trace=None,
):
    log_dir = output_dir.parent / "logs"
    deadline = time.monotonic() + timeout_seconds
    started = time.perf_counter()
    started_epoch = time.time()
    last_files = []
    last_count = 0
    last_source = "none"
    poll_count = 0
    first_file_seen_seconds = None
    first_manifest_count_seconds = None
    first_hdf5_count_seconds = None
    first_complete_count_seconds = None
    poll_records = []
    poll_interval_seconds = max(0.001, float(poll_interval_seconds))
    while time.monotonic() < deadline:
        poll_count += 1
        files = sorted(output_dir.glob(f"{chronicle}.{story}*.h5"))
        if files and first_file_seen_seconds is None:
            first_file_seen_seconds = time.perf_counter() - started
        manifest_started = time.perf_counter()
        manifest_total = manifest_archived_event_count(files)
        manifest_seconds = time.perf_counter() - manifest_started
        hdf5_started = time.perf_counter()
        hdf5_total = None if manifest_total is not None else hdf5_archived_event_count(files)
        hdf5_seconds = time.perf_counter() - hdf5_started
        if manifest_total is not None:
            total = manifest_total
            source = "archive_manifest"
            grapher_seconds = 0.0
            if first_manifest_count_seconds is None:
                first_manifest_count_seconds = time.perf_counter() - started
        elif hdf5_total is not None:
            total = hdf5_total
            source = "hdf5_dataset"
            grapher_seconds = 0.0
            if first_hdf5_count_seconds is None:
                first_hdf5_count_seconds = time.perf_counter() - started
        else:
            grapher_started = time.perf_counter()
            total = grapher_archived_event_count(log_dir, chronicle, story)
            grapher_seconds = time.perf_counter() - grapher_started
            source = "grapher_log"
        last_files = files
        last_count = total
        last_source = source
        elapsed_seconds = time.perf_counter() - started
        poll_records.append(
            {
                "poll_index": poll_count,
                "elapsed_seconds": elapsed_seconds,
                "file_count": len(files),
                "count": total,
                "source": source,
                "manifest_count_seconds": manifest_seconds,
                "hdf5_count_seconds": hdf5_seconds,
                "grapher_log_count_seconds": grapher_seconds,
            }
        )
        if files and total >= expected_count:
            first_complete_count_seconds = elapsed_seconds
            if trace is not None:
                trace.update(
                    {
                        "chronicle": chronicle,
                        "story": story,
                        "expected_count": expected_count,
                        "started_epoch_seconds": started_epoch,
                        "poll_interval_seconds": poll_interval_seconds,
                        "poll_count": poll_count,
                        "first_file_seen_seconds": first_file_seen_seconds,
                        "first_manifest_count_seconds": first_manifest_count_seconds,
                        "first_hdf5_count_seconds": first_hdf5_count_seconds,
                        "first_complete_count_seconds": first_complete_count_seconds,
                        "confirm_seconds": elapsed_seconds,
                        "last_count": last_count,
                        "last_source": last_source,
                        "last_file_count": len(last_files),
                        "hdf5_count_seconds_total": sum(
                            record["hdf5_count_seconds"] for record in poll_records
                        ),
                        "hdf5_count_seconds_max": max(
                            (record["hdf5_count_seconds"] for record in poll_records),
                            default=0.0,
                        ),
                        "manifest_count_seconds_total": sum(
                            record["manifest_count_seconds"] for record in poll_records
                        ),
                        "manifest_count_seconds_max": max(
                            (record["manifest_count_seconds"] for record in poll_records),
                            default=0.0,
                        ),
                        "grapher_log_count_seconds_total": sum(
                            record["grapher_log_count_seconds"] for record in poll_records
                        ),
                        "grapher_log_count_seconds_max": max(
                            (record["grapher_log_count_seconds"] for record in poll_records),
                            default=0.0,
                        ),
                        "poll_records": poll_records,
                    }
                )
            return files, total, source, poll_count
        time.sleep(poll_interval_seconds)
    if trace is not None:
        trace.update(
            {
                "chronicle": chronicle,
                "story": story,
                "expected_count": expected_count,
                "started_epoch_seconds": started_epoch,
                "poll_interval_seconds": poll_interval_seconds,
                "poll_count": poll_count,
                "first_file_seen_seconds": first_file_seen_seconds,
                "first_manifest_count_seconds": first_manifest_count_seconds,
                "first_hdf5_count_seconds": first_hdf5_count_seconds,
                "first_complete_count_seconds": first_complete_count_seconds,
                "confirm_seconds": time.perf_counter() - started,
                "last_count": last_count,
                "last_source": last_source,
                "last_file_count": len(last_files),
                "hdf5_count_seconds_total": sum(record["hdf5_count_seconds"] for record in poll_records),
                "hdf5_count_seconds_max": max(
                    (record["hdf5_count_seconds"] for record in poll_records),
                    default=0.0,
                ),
                "manifest_count_seconds_total": sum(record["manifest_count_seconds"] for record in poll_records),
                "manifest_count_seconds_max": max(
                    (record["manifest_count_seconds"] for record in poll_records),
                    default=0.0,
                ),
                "grapher_log_count_seconds_total": sum(
                    record["grapher_log_count_seconds"] for record in poll_records
                ),
                "grapher_log_count_seconds_max": max(
                    (record["grapher_log_count_seconds"] for record in poll_records),
                    default=0.0,
                ),
                "poll_records": poll_records,
            }
        )
    raise RuntimeError(
        f"Timed out waiting for {expected_count} archived events in {output_dir}; "
        f"last_count={last_count}, last_source={last_source}, poll_count={poll_count}, "
        f"poll_interval_seconds={poll_interval_seconds}, files={[str(path) for path in last_files]}"
    )


def archive_event_count_wait_trace_metrics(traces):
    metrics = {
        "archive_event_count_wait_story_count": len(traces),
        "archive_event_count_wait_poll_count_total": sum(trace.get("poll_count") or 0 for trace in traces),
    }
    numeric_fields = (
        "confirm_seconds",
        "first_file_seen_seconds",
        "first_manifest_count_seconds",
        "first_hdf5_count_seconds",
        "first_complete_count_seconds",
        "manifest_count_seconds_total",
        "manifest_count_seconds_max",
        "hdf5_count_seconds_total",
        "hdf5_count_seconds_max",
        "grapher_log_count_seconds_total",
        "grapher_log_count_seconds_max",
    )
    for field in numeric_fields:
        values = [trace.get(field) for trace in traces if trace.get(field) is not None]
        prefix = f"archive_event_count_wait_{field}"
        if values:
            metrics[f"{prefix}_min"] = min(values)
            metrics[f"{prefix}_max"] = max(values)
            metrics[f"{prefix}_avg"] = statistics.fmean(values)
        else:
            metrics[f"{prefix}_min"] = None
            metrics[f"{prefix}_max"] = None
            metrics[f"{prefix}_avg"] = None
    return metrics


def run_client_append(config_path, result_dir, chronicle, story, operation_count, message_size_bytes):
    import py_chronolog_client

    phase_started = time.perf_counter()
    phase_started_epoch = time.time()
    phase_marks = {
        "client_phase_started_epoch_seconds": phase_started_epoch,
    }

    def mark_phase(name):
        now = time.perf_counter()
        phase_marks[f"client_phase_{name}_seconds"] = now - phase_started
        phase_marks[f"client_phase_{name}_epoch_seconds"] = phase_started_epoch + (now - phase_started)

    config = json.loads(Path(config_path).read_text())
    mark_phase("config_loaded")
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
    progress_path = Path(result_dir) / "chronolog" / "durable-client-progress.json"
    payload = "d" * message_size_bytes
    attrs = {}
    flags = 1
    append_latencies_ms = []
    ret = client.Connect()
    mark_phase("connect_returned")
    if ret != 0:
        raise RuntimeError(f"Connect returned {ret}")
    ret = client.CreateChronicle(chronicle, attrs, flags)
    mark_phase("create_chronicle_returned")
    if ret != 0:
        raise RuntimeError(f"CreateChronicle returned {ret}")
    ret, handle = client.AcquireStory(chronicle, story, attrs, flags)
    mark_phase("acquire_story_returned")
    if ret != 0 or handle is None:
        raise RuntimeError(f"AcquireStory returned {ret}")
    mark_phase("append_loop_started")
    for index in range(operation_count):
        op_started = time.perf_counter()
        handle.log_event(f"{index}:{payload}")
        append_latencies_ms.append((time.perf_counter() - op_started) * 1000)
    mark_phase("append_loop_finished")
    progress = {
        "chronicle": chronicle,
        "story": story,
        "append_latencies_ms": append_latencies_ms,
        "release_started": time.time(),
        **phase_marks,
    }
    progress_path.write_text(json.dumps(progress, indent=2) + "\n", encoding="utf-8")
    release_ret = client.ReleaseStory(chronicle, story)
    mark_phase("release_story_returned")
    progress["release_returned"] = time.time()
    progress["release_ret"] = release_ret
    progress.update(phase_marks)
    progress_path.write_text(json.dumps(progress, indent=2) + "\n", encoding="utf-8")
    if release_ret != 0:
        raise RuntimeError(f"ReleaseStory returned {release_ret}")


def main():
    parser = argparse.ArgumentParser(description="Run a ChronoLog durable append completion benchmark.")
    parser.add_argument("--config", required=True)
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--operation-count", type=int, default=10)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--node-count", type=int, default=2)
    parser.add_argument("--client-count", type=int, default=1)
    parser.add_argument("--archive-wait-seconds", type=float, default=420.0)
    parser.add_argument("--archive-event-count-poll-interval-seconds", type=float, default=1.0)
    parser.add_argument("--completion-mode", choices=["archive_readback"], default="archive_readback")
    args = parser.parse_args()
    if args.client_count != 1:
        raise SystemExit("chronolog append_durable currently supports client-count=1 only")

    result_root = Path(args.result_dir)
    chronolog_dir = result_root / "chronolog"
    output_dir = chronolog_dir / "output"
    chronolog_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"{int(time.time() * 1000)}_{os.getpid()}"
    chronicle = f"phase0_durable_chronicle_{suffix}"
    story = f"phase0_durable_story_{suffix}"
    append_latencies_ms = []
    progress_path = chronolog_dir / "durable-client-progress.json"

    started = time.perf_counter()
    child = multiprocessing.Process(
        target=run_client_append,
        args=(args.config, str(result_root), chronicle, story, args.operation_count, args.message_size_bytes),
    )
    child.start()
    archive_started = time.perf_counter()
    archive_started_epoch = time.time()
    archive_files = []
    archive_count = 0
    archive_file_detect = None
    archive_file_detect_epoch = None
    archive_count_confirm = None
    archive_count_confirm_epoch = None
    archive_count_poll_count = None
    archive_count_wait_trace = {}
    archive_error = ""
    try:
        archive_files = wait_for_archive_file(output_dir, chronicle, story, args.archive_wait_seconds)
        archive_file_detect = time.perf_counter() - archive_started
        archive_file_detect_epoch = time.time()
        archive_count_started = time.perf_counter()
        archive_files, archive_count, readback_path, archive_count_poll_count = wait_for_archive_event_count(
            output_dir,
            chronicle,
            story,
            args.operation_count,
            args.archive_wait_seconds,
            args.archive_event_count_poll_interval_seconds,
            archive_count_wait_trace,
        )
        archive_count_confirm = time.perf_counter() - archive_count_started
        archive_count_confirm_epoch = time.time()
    except Exception as exc:
        archive_error = str(exc)
        archive_files = sorted(output_dir.glob(f"{chronicle}.{story}*.h5"))
        hdf5_count = hdf5_archived_event_count(archive_files)
        if hdf5_count is not None:
            archive_count = hdf5_count
            readback_path = "hdf5_dataset"
        else:
            archive_count = grapher_archived_event_count(chronolog_dir / "logs", chronicle, story)
            readback_path = "grapher_hdf5_event_count_log"
        if archive_file_detect is None and archive_files:
            archive_file_detect = time.perf_counter() - archive_started
            archive_file_detect_epoch = time.time()
        if archive_count_confirm is None:
            archive_count_confirm = time.perf_counter() - archive_started
            archive_count_confirm_epoch = time.time()
    readback_count = archive_count
    if progress_path.exists():
        try:
            progress = json.loads(progress_path.read_text(encoding="utf-8"))
            append_latencies_ms = progress.get("append_latencies_ms", [])
        except Exception:
            progress = {}
            append_latencies_ms = []
    else:
        progress = {}
    child.join(timeout=5)
    release_returned = not child.is_alive()
    if child.is_alive():
        child.terminate()
        child.join(timeout=10)
    duration = time.perf_counter() - started
    archive_wait = (time.perf_counter() - archive_started) if archive_started is not None else None
    total_operation_count = args.operation_count * args.client_count
    throughput = total_operation_count / duration if duration > 0 else 0.0
    success = archive_count >= args.operation_count and readback_count >= args.operation_count
    release_started = progress.get("release_started")
    release_returned_at = progress.get("release_returned")
    time.sleep(log_settle_seconds())
    control_metrics = wait_for_control_path_profiles(chronolog_dir / "logs", args.client_count)
    grapher_stage_times, story_id = grapher_archive_stage_times(chronolog_dir / "logs", chronicle, story)
    grapher_stage_metrics = grapher_archive_stage_metrics(
        chronolog_dir / "logs", [(chronicle, story)], release_returned_at
    )
    release_to_stage_seconds = {
        f"release_to_{stage}_seconds": elapsed_since(release_returned_at, timestamp)
        for stage, timestamp in grapher_stage_times.items()
    }
    file_stat_metrics = archive_file_stat_metrics(archive_files, release_returned_at, archive_file_detect_epoch)
    keeper_drain_metrics = keeper_to_grapher_drain_metrics(chronolog_dir / "logs", story_id, release_returned_at)
    grapher_orphan_metrics = grapher_orphan_chunk_metrics(chronolog_dir / "logs", story_id, release_returned_at)
    metrics = {
        "system": "chronolog",
        "workflow": "append_durable",
        "node_count": args.node_count,
        "nodes": args.node_count,
        "client_count": args.client_count,
        "parallel_clients": args.client_count,
        "message_size_bytes": args.message_size_bytes,
        "operation_count": args.operation_count,
        "operation_count_per_client": args.operation_count,
        "message_count_per_client": args.operation_count,
        "messages_per_client": args.operation_count,
        "total_operation_count": total_operation_count,
        "total_message_count": total_operation_count,
        "total_messages": total_operation_count,
        "total_payload_bytes": total_operation_count * args.message_size_bytes,
        "parallel_client_count": args.client_count,
        "duration_seconds": duration,
        "throughput_ops_per_sec": throughput,
        "avg_latency_ms": statistics.fmean(append_latencies_ms) if append_latencies_ms else None,
        "p50_latency_ms": percentile(append_latencies_ms, 50),
        "p95_latency_ms": percentile(append_latencies_ms, 95),
        "p99_latency_ms": percentile(append_latencies_ms, 99),
        "success": success,
        "chronolog_completion_mode": args.completion_mode,
        "archive_log_settle_seconds": log_settle_seconds(),
        "chronolog_grapher_inactive_story_delay_seconds": os.environ.get(
            "CHRONOLOG_GRAPHER_INACTIVE_STORY_DELAY_SECONDS_EFFECTIVE", ""
        ),
        "durability_boundary": "archive_file_event_count_and_readback",
        "archive_wait_seconds": archive_wait,
        "archive_wait_started_epoch_seconds": archive_started_epoch,
        "archive_file_detect_seconds": archive_file_detect,
        "archive_file_detect_epoch_seconds": archive_file_detect_epoch,
        "archive_event_count_confirm_seconds": archive_count_confirm,
        "archive_event_count_confirm_epoch_seconds": archive_count_confirm_epoch,
        "archive_event_count_poll_interval_seconds": args.archive_event_count_poll_interval_seconds,
        "archive_event_count_poll_count": archive_count_poll_count,
        "archive_event_count_wait_trace": archive_count_wait_trace,
        **archive_event_count_wait_trace_metrics([archive_count_wait_trace] if archive_count_wait_trace else []),
        "archive_event_count": archive_count,
        "archive_files": [str(path) for path in archive_files],
        **file_stat_metrics,
        "archive_error": archive_error or None,
        "readback_event_count": readback_count,
        "readback_path": readback_path,
        "release_returned_before_metrics": release_returned,
        "release_seconds": elapsed_since(release_started, release_returned_at),
        "release_started_epoch_seconds": release_started,
        "release_returned_epoch_seconds": release_returned_at,
        "story_id": story_id,
        "grapher_archive_stage_epoch_seconds": grapher_stage_times,
        **grapher_stage_metrics,
        **grapher_hdf5_write_profile_metrics(chronolog_dir / "logs"),
        **control_metrics,
        **keeper_drain_metrics,
        **grapher_orphan_metrics,
        "grapher_finalize_to_hdf5_written_seconds": elapsed_since(
            grapher_stage_times.get("grapher_pipeline_finalized"),
            grapher_stage_times.get("grapher_hdf5_written"),
        ),
        **release_to_stage_seconds,
        "client_exitcode": child.exitcode,
        "readback_note": "In-job readback count uses the HDF5 story_chunks/data.vlen_bytes, story_chunks/data.meta, story_chunks/data.raw_meta, or story_chunks/data.fixed_meta dataset when h5py can read it, with Grapher HDF5FileChunkExtractor eventCount logs as fallback.",
    }
    (chronolog_dir / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    if not success:
        raise SystemExit(1)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"chronolog durable append benchmark failed: {exc}", file=sys.stderr)
        raise
