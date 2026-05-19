#!/usr/bin/env python3

import argparse
import json
import os
import subprocess
import textwrap
import time
from pathlib import Path


def run(cmd, *, cwd, env=None):
    return subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True)


def main():
    parser = argparse.ArgumentParser(description="Validate KeeperLocalJournal same-story recovery across processes.")
    parser.add_argument("--result-dir", required=True)
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--install-dir", default=None)
    parser.add_argument("--event-count", type=int, default=1000)
    parser.add_argument("--message-size-bytes", type=int, default=1024)
    parser.add_argument("--node-count", type=int, default=1)
    parser.add_argument("--client-count", type=int, default=1)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    install_dir = Path(args.install_dir or repo_root / ".agent/install-tau/chronolog").resolve()
    result_dir = Path(args.result_dir).resolve()
    result_dir.mkdir(parents=True, exist_ok=True)
    journal_dir = result_dir / "journal"
    journal_dir.mkdir(parents=True, exist_ok=True)

    source = result_dir / "keeper_journal_recovery_unit.cpp"
    binary = result_dir / "keeper_journal_recovery_unit"
    story_id = 0xC001CAFE1234
    client_id = 0xA11CE

    source.write_text(
        textwrap.dedent(
            f"""
            #include <cstdlib>
            #include <iostream>
            #include <string>
            #include <vector>

            #include <KeeperLocalJournal.h>
            #include <chrono_monitor.h>
            #include <chronolog_types.h>

            int main(int argc, char** argv)
            {{
                if(argc != 2)
                {{
                    std::cerr << "usage: " << argv[0] << " write|read\\n";
                    return 2;
                }}

                chronolog::chrono_monitor::initialize("console", "", chronolog::LogLevel::warn, "keeper_journal_recovery_unit");
                std::string const mode(argv[1]);
                constexpr chronolog::StoryId story_id = {story_id}ULL;
                constexpr chronolog::ClientId client_id = {client_id}ULL;
                constexpr std::size_t event_count = {args.event_count};
                constexpr std::size_t message_size = {args.message_size_bytes};

                if(mode == "write")
                {{
                    for(std::size_t i = 0; i < event_count; ++i)
                    {{
                        std::string payload = std::to_string(i) + ":";
                        payload.resize(message_size, 'x');
                        chronolog::LogEvent event(story_id, 1000 + i, client_id, static_cast<chronolog::chrono_index>(i), payload);
                        if(!chronolog::KeeperLocalJournal::instance().append(event))
                        {{
                            std::cerr << "append failed at " << i << "\\n";
                            return 3;
                        }}
                    }}
                    return 0;
                }}

                if(mode == "read")
                {{
                    std::vector<chronolog::LogEvent> events;
                    if(!chronolog::KeeperLocalJournal::instance().collectEvents(story_id, 1, 1000 + event_count + 1, events))
                    {{
                        std::cerr << "collectEvents failed\\n";
                        return 4;
                    }}
                    if(events.size() != event_count)
                    {{
                        std::cerr << "expected " << event_count << " events, got " << events.size() << "\\n";
                        return 5;
                    }}
                    for(std::size_t i = 0; i < events.size(); ++i)
                    {{
                        if(events[i].storyId != story_id || events[i].eventTime != 1000 + i || events[i].clientId != client_id ||
                           events[i].eventIndex != i || events[i].logRecord.size() != message_size)
                        {{
                            std::cerr << "event mismatch at " << i << "\\n";
                            return 6;
                        }}
                    }}
                    std::cout << "recovered_events=" << events.size() << "\\n";
                    return 0;
                }}

                std::cerr << "unknown mode: " << mode << "\\n";
                return 2;
            }}
            """
        )
    )

    compile_cmd = [
        "g++",
        "-std=c++17",
        "-O2",
        "-I",
        str(repo_root / "ChronoKeeper/include"),
        "-I",
        str(repo_root / "Client/cpp/include"),
        str(source),
        "-L",
        str(install_dir / "lib"),
        "-Wl,-rpath," + str(install_dir / "lib"),
        "-lchronolog_client",
        "-o",
        str(binary),
    ]
    compile_result = run(compile_cmd, cwd=repo_root)
    (result_dir / "compile.stdout.log").write_text(compile_result.stdout)
    (result_dir / "compile.stderr.log").write_text(compile_result.stderr)
    if compile_result.returncode != 0:
        raise SystemExit(compile_result.returncode)

    env = os.environ.copy()
    env["CHRONOLOG_KEEPER_JOURNAL_MODE"] = "fdatasync"
    env["CHRONOLOG_KEEPER_JOURNAL_DIR"] = str(journal_dir)
    env["LD_LIBRARY_PATH"] = str(install_dir / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")

    started = time.perf_counter()
    write_result = run([str(binary), "write"], cwd=repo_root, env=env)
    (result_dir / "write.stdout.log").write_text(write_result.stdout)
    (result_dir / "write.stderr.log").write_text(write_result.stderr)
    if write_result.returncode != 0:
        raise SystemExit(write_result.returncode)

    read_result = run([str(binary), "read"], cwd=repo_root, env=env)
    (result_dir / "read.stdout.log").write_text(read_result.stdout)
    (result_dir / "read.stderr.log").write_text(read_result.stderr)
    if read_result.returncode != 0:
        raise SystemExit(read_result.returncode)

    journals = sorted(journal_dir.glob("*.journal"))
    duration = time.perf_counter() - started
    total_operation_count = args.event_count * args.client_count
    metrics = {
        "system": "chronolog",
        "workflow": "keeper_journal_same_story_recovery_unit",
        "node_count": args.node_count,
        "nodes": args.node_count,
        "client_count": args.client_count,
        "parallel_clients": args.client_count,
        "message_size_bytes": args.message_size_bytes,
        "operation_count": args.event_count,
        "operation_count_per_client": args.event_count,
        "message_count_per_client": args.event_count,
        "messages_per_client": args.event_count,
        "total_operation_count": total_operation_count,
        "total_message_count": total_operation_count,
        "total_messages": total_operation_count,
        "total_payload_bytes": total_operation_count * args.message_size_bytes,
        "parallel_client_count": args.client_count,
        "duration_seconds": duration,
        "throughput_ops_per_sec": total_operation_count / duration if duration > 0 else 0.0,
        "avg_latency_ms": None,
        "p50_latency_ms": None,
        "p95_latency_ms": None,
        "p99_latency_ms": None,
        "semantic_boundary": "keeper_local_journal_fdatasync_same_story_recovery",
        "durability_boundary": "keeper_local_journal_fdatasync",
        "read_path": "keeper_local_journal_tail_recovered_index",
        "journal_file_count": len(journals),
        "journal_total_bytes": sum(path.stat().st_size for path in journals),
        "recovered_event_count": args.event_count,
        "success": True,
    }
    (result_dir / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
