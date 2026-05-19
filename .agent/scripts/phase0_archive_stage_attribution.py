#!/usr/bin/env python3
"""Summarize ChronoLog archive/range stage timing from a metrics.json file."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Any


def as_float(value: Any) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def fmt(value: Any, digits: int = 3) -> str:
    number = as_float(value)
    if number is None:
        return ""
    return f"{number:.{digits}f}"


def metric_rows(metrics: dict[str, Any], rows: list[tuple[str, str, str]]) -> list[dict[str, Any]]:
    output = []
    for label, unit, key in rows:
        value = metrics.get(key)
        if as_float(value) is not None:
            output.append({"stage": label, "unit": unit, "value": value, "metric": key})
    return output


def append_table(md: list[str], title: str, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    md.extend(["", f"## {title}", "", "| Stage | Unit | Value | Metric |", "|---|---:|---:|---|"])
    for row in rows:
        md.append(f"| {row['stage']} | {row['unit']} | {fmt(row['value'])} | `{row['metric']}` |")


def wrapper_identity(metrics_path: Path) -> dict[str, str]:
    wrapper = metrics_path.parent / "wrappers" / "chrono-grapher"
    if not wrapper.exists():
        return {}
    match = re.search(r"^real_bin=(.+)$", wrapper.read_text(encoding="utf-8", errors="replace"), re.MULTILINE)
    if not match:
        return {}
    real_bin = match.group(1).strip().strip("'\"")
    bin_path = Path(real_bin)
    bin_dir = bin_path.parent
    install_dir = bin_dir.parent
    return {
        "chronolog_install_dir": str(install_dir),
        "chronolog_bin_dir": str(bin_dir),
        "chronolog_deploy_script": str(install_dir / "tools" / "deploy" / "deploy_cluster.sh"),
        "chronolog_benchmark_bin": str(install_dir / "tools" / "benchmark" / "chrono-bench"),
    }


def identity_rows(metrics: dict[str, Any], metrics_path: Path) -> list[dict[str, Any]]:
    fallback = wrapper_identity(metrics_path)
    rows = []
    for label, key in [
        ("ChronoLog install", "chronolog_install_dir"),
        ("ChronoLog binary dir", "chronolog_bin_dir"),
        ("Deploy script", "chronolog_deploy_script"),
        ("Benchmark binary", "chronolog_benchmark_bin"),
    ]:
        source = "metrics"
        value = metrics.get(key)
        if not value and fallback.get(key):
            source = "wrapper"
            value = fallback[key]
        if value:
            rows.append({"label": label, "value": value, "metric": key, "source": source})
    return rows


def write_outputs(metrics_path: Path, output_dir: Path) -> None:
    metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
    output_dir.mkdir(parents=True, exist_ok=True)
    run_identity_rows = identity_rows(metrics, metrics_path)

    overview_rows = metric_rows(metrics, [
        ("active workflow", "seconds", "workflow_active_duration_seconds"),
        ("append clients", "seconds", "archive_range_append_clients_seconds"),
        ("client release max", "seconds", "client_phase_release_story_returned_seconds_max"),
        ("archive publication confirm", "seconds", "archive_publication_confirm_seconds"),
        ("archive count confirm", "seconds", "archive_event_count_confirm_seconds"),
        ("range readback", "seconds", "range_readback_duration_seconds"),
    ])
    release_rows = metric_rows(metrics, [
        ("Visor release total max", "us", "visor_release_story_total_us_max"),
        ("Visor release notify RecordingService max", "us", "visor_release_story_notify_recording_stop_us_max"),
        ("RecordingService stop total max", "us", "registry_recording_stop_total_us_max"),
        ("RecordingService Keeper notify max", "us", "registry_recording_stop_keeper_notify_us_max"),
        ("RecordingService Grapher notify max", "us", "registry_recording_stop_grapher_notify_us_max"),
        ("Registry Keeper-stop RPC total max", "us", "registry_keeper_stop_rpc_total_us_max"),
        ("Registry Keeper-stop per-target RPC max", "us", "registry_keeper_stop_rpc_rpc_max_us_max"),
        ("Registry Keeper-stop RPC sum max", "us", "registry_keeper_stop_rpc_rpc_sum_us_max"),
        ("Registry Grapher-stop RPC total max", "us", "registry_grapher_stop_rpc_total_us_max"),
        ("Registry Grapher-stop expected Keeper drains max", "count", "registry_grapher_stop_rpc_expected_keeper_drains_max"),
    ])
    keeper_rows = metric_rows(metrics, [
        ("Keeper StopStory total max", "us", "keeper_stop_story_total_us_max"),
        ("Keeper StopStory flush story max", "us", "keeper_stop_story_flush_story_us_max"),
        ("Keeper StopStory datastore stop max", "us", "keeper_stop_story_datastore_stop_us_max"),
        ("Keeper StopStory async drain idle max", "us", "keeper_stop_story_async_drain_idle_us_max"),
        ("Keeper StopStory profile count", "count", "keeper_stop_story_profile_count"),
    ])
    grapher_rows = metric_rows(metrics, [
        ("Grapher archive drain count", "count", "grapher_archive_drain_count"),
        ("Grapher archive drain success count", "count", "grapher_archive_drain_success_count"),
        ("Grapher archive drain queued nonzero count", "count", "grapher_archive_drain_queued_nonzero_count"),
        ("Grapher archive drain wait max", "us", "grapher_archive_drain_wait_max_us"),
        ("Grapher archive drain finalize max", "us", "grapher_archive_drain_finalize_max_us"),
        ("Grapher stop/retire profile count", "count", "grapher_stop_retire_profile_count"),
        ("Grapher stop/retire total max", "us", "grapher_stop_retire_total_max_us"),
        ("Grapher stop/retire completion wait max", "us", "grapher_stop_retire_completion_wait_max_us"),
        ("Grapher stop/retire collect/erase max", "us", "grapher_stop_retire_collect_erase_max_us"),
        ("Grapher stop/retire finalize max", "us", "grapher_stop_retire_finalize_max_us"),
        ("Grapher stop/retire archive-drain wait max", "us", "grapher_stop_retire_archive_drain_wait_max_us"),
        ("Grapher stop/retire initial lock wait max", "us", "grapher_stop_retire_initial_lock_wait_max_us"),
        ("Grapher stop/retire initial lock hold max", "us", "grapher_stop_retire_initial_lock_hold_max_us"),
    ])
    writer_rows = metric_rows(metrics, [
        ("HDF5 writer total max", "us", "grapher_hdf5_writer_total_us_max"),
        ("HDF5 writer total avg", "us", "grapher_hdf5_writer_total_us_avg"),
        ("HDF5 lock wait max", "us", "grapher_hdf5_lock_wait_us_max"),
        ("HDF5 open max", "us", "grapher_hdf5_open_us_max"),
        ("HDF5 write call max", "us", "grapher_hdf5_dataset_write_call_us_max"),
        ("HDF5 raw payload writev max", "us", "grapher_hdf5_raw_payload_writev_us_max"),
        ("HDF5 dataset metadata write max", "us", "grapher_hdf5_dataset_write_us_max"),
        ("HDF5 close max", "us", "grapher_hdf5_close_us_max"),
        ("HDF5 write profile count", "count", "grapher_hdf5_write_profile_count"),
    ])
    publisher_rows = metric_rows(metrics, [
        ("Async publisher count", "count", "grapher_async_archive_publish_count"),
        ("Async publisher success count", "count", "grapher_async_archive_publish_success_count"),
        ("Async publisher close wait max", "us", "grapher_async_archive_publish_close_wait_us_max"),
        ("Async publisher close max", "us", "grapher_async_archive_publish_close_us_max"),
        ("Async publisher rename max", "us", "grapher_async_archive_publish_publish_rename_us_max"),
        ("Async publisher manifest write max", "us", "grapher_async_archive_publish_archive_manifest_write_us_max"),
        ("Async publisher total max", "us", "grapher_async_archive_publish_total_us_max"),
    ])

    publication_rows = []
    for item in metrics.get("archive_publication_results") or []:
        publication_rows.append(
            {
                "client_index": item.get("client_index"),
                "story": item.get("story"),
                "publication_confirm_seconds": item.get("archive_publication_confirm_seconds"),
                "file_count": item.get("archive_publication_file_count"),
            }
        )
    publication_rows.sort(key=lambda row: as_float(row["publication_confirm_seconds"]) or -1, reverse=True)

    wait_trace_rows = []
    for item in metrics.get("archive_event_count_wait_traces") or []:
        wait_trace_rows.append(
            {
                "story": item.get("story"),
                "expected_count": item.get("expected_count"),
                "confirm_seconds": item.get("confirm_seconds"),
                "first_file_seen_seconds": item.get("first_file_seen_seconds"),
                "first_manifest_count_seconds": item.get("first_manifest_count_seconds"),
                "last_source": item.get("last_source"),
                "poll_count": item.get("poll_count"),
            }
        )
    wait_trace_rows.sort(key=lambda row: as_float(row["confirm_seconds"]) or -1, reverse=True)

    profile_root = metrics_path.parent / "profiles"
    profiles = [p for p in profile_root.rglob("*") if p.is_file()] if profile_root.exists() else []
    nonempty_profiles = [p for p in profiles if p.stat().st_size > 0]

    sections = {
        "overview": overview_rows,
        "release_fanout": release_rows,
        "keeper_stop_story": keeper_rows,
        "grapher_archive_drain": grapher_rows,
        "hdf5_writer": writer_rows,
        "async_publisher": publisher_rows,
    }
    summary = {
        "metrics_path": str(metrics_path),
        "system": metrics.get("system"),
        "workflow": metrics.get("workflow"),
        "node_count": metrics.get("node_count"),
        "client_count": metrics.get("client_count"),
        "message_size_bytes": metrics.get("message_size_bytes"),
        "operation_count": metrics.get("operation_count"),
        "success": metrics.get("success"),
        "sections": sections,
        "stage_rows": [row for section_rows in sections.values() for row in section_rows],
        "publication_rows": publication_rows,
        "archive_event_count_wait_traces": wait_trace_rows,
        "profile_file_count": len(profiles),
        "profile_nonempty_count": len(nonempty_profiles),
        "profile_total_bytes": sum(p.stat().st_size for p in nonempty_profiles),
        "run_identity": run_identity_rows,
    }

    (output_dir / "archive-stage-attribution.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    md = [
        "# Archive Stage Attribution",
        "",
        f"- metrics: `{metrics_path}`",
        f"- system/workflow: `{metrics.get('system')}` / `{metrics.get('workflow')}`",
        f"- nodes/clients: `{metrics.get('node_count')}` / `{metrics.get('client_count')}`",
        f"- message_size_bytes: `{metrics.get('message_size_bytes')}`",
        f"- operation_count: `{metrics.get('operation_count')}`",
        f"- success: `{metrics.get('success')}`",
        f"- profile artifacts: `{len(nonempty_profiles)}/{len(profiles)}` nonempty, `{summary['profile_total_bytes']}` bytes",
        "",
    ]
    if run_identity_rows:
        md.extend(["## Run Identity", "", "| Field | Value | Metric | Source |", "|---|---|---|---|"])
        for row in run_identity_rows:
            md.append(f"| {row['label']} | `{row['value']}` | `{row['metric']}` | `{row['source']}` |")
    append_table(md, "Overview", overview_rows)
    append_table(md, "Release Fanout", release_rows)
    append_table(md, "Keeper StopStory", keeper_rows)
    append_table(md, "Grapher Archive Drain", grapher_rows)
    append_table(md, "HDF5 Writer", writer_rows)
    append_table(md, "Async Publisher", publisher_rows)

    md.extend(["", "## Publication Skew", "", "| Client | Story | Confirm s | Files |", "|---:|---|---:|---:|"])
    for row in publication_rows:
        md.append(
            f"| {row['client_index']} | `{row['story']}` | "
            f"{fmt(row['publication_confirm_seconds'], 6)} | {row['file_count']} |"
        )

    if wait_trace_rows:
        md.extend([
            "",
            "## Archive Count Wait Traces",
            "",
            "| Story | Expected | Confirm s | First File s | First Manifest Count s | Polls | Source |",
            "|---|---:|---:|---:|---:|---:|---|",
        ])
        for row in wait_trace_rows:
            md.append(
                f"| `{row['story']}` | {row['expected_count']} | {fmt(row['confirm_seconds'], 6)} | "
                f"{fmt(row['first_file_seen_seconds'], 6)} | "
                f"{fmt(row['first_manifest_count_seconds'], 6)} | {row['poll_count']} | `{row['last_source']}` |"
            )

    (output_dir / "archive-stage-attribution.md").write_text("\n".join(md) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--metrics", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    write_outputs(args.metrics, args.output_dir)
    print(args.output_dir / "archive-stage-attribution.md")
    print(args.output_dir / "archive-stage-attribution.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
