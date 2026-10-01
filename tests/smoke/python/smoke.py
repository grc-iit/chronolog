#!/usr/bin/env python3
"""Smoke test for a running ChronoLog compose stack.

Generates stubs from proto/chronolog/v1 with gen_stubs.sh, then drives the Catalog
on the Visor: create chronicle, create story, acquire twice (incarnation 1 then 2).
Append, read and tail are placeholders until the Keeper and Player images ship the
real binaries. Exits 0 only if every active step passes.
"""

import argparse
import importlib
import os
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_VISOR = "127.0.0.1:50051"


def generate_stubs(out_dir: Path) -> None:
    subprocess.run([str(HERE / "gen_stubs.sh"), str(out_dir)], check=True, env={**os.environ, "PYTHON": sys.executable})


class Smoke:
    def __init__(self, visor: str, rpc_timeout: float):
        import grpc

        sys.path.insert(0, str(self.stubs))
        self.pb = importlib.import_module("chronolog.v1.chronolog_pb2")
        self.rpc = importlib.import_module("chronolog.v1.chronolog_pb2_grpc")
        self.grpc = grpc
        self.channel = grpc.insecure_channel(visor)
        self.catalog = self.rpc.CatalogStub(self.channel)
        self.timeout = rpc_timeout
        self.failures = 0

    stubs: Path = Path()

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        print(f"{'PASS' if ok else 'FAIL'} {name}{(' ' + detail) if detail else ''}")
        if not ok:
            self.failures += 1
        return ok

    def skip(self, name: str, reason: str) -> None:
        print(f"SKIP {name} ({reason})")

    def wait_ready(self, seconds: float) -> bool:
        try:
            self.grpc.channel_ready_future(self.channel).result(timeout=seconds)
            return True
        except self.grpc.FutureTimeoutError:
            return False

    def catalog_steps(self) -> None:
        suffix = uuid.uuid4().hex[:8]
        chronicle, story_name, writer = f"smoke-{suffix}", "s1", f"smoke-writer-{suffix}"
        pb, timeout = self.pb, self.timeout

        r = self.catalog.CreateChronicle(pb.CreateChronicleRequest(name=chronicle), timeout=timeout)
        if not self.check("CreateChronicle", r.status.code == 0 and r.chronicle.name == chronicle, r.status.message):
            return
        r = self.catalog.CreateStory(pb.CreateStoryRequest(chronicle=chronicle, name=story_name), timeout=timeout)
        if not self.check("CreateStory", r.status.code == 0 and r.story.story_id != 0, r.status.message):
            return
        story_id = r.story.story_id

        first = self.catalog.Acquire(pb.AcquireRequest(story_id=story_id, writer_identity=writer), timeout=timeout)
        ok = first.status.code == 0 and first.incarnation == 1 and first.writer_id != 0
        self.check("Acquire #1 incarnation 1", ok, f"incarnation={first.incarnation} {first.status.message}")
        second = self.catalog.Acquire(pb.AcquireRequest(story_id=story_id, writer_identity=writer), timeout=timeout)
        ok = (second.status.code == 0 and second.incarnation == 2 and second.writer_id == first.writer_id)
        self.check("Acquire #2 incarnation 2 (same writer_id)", ok, f"incarnation={second.incarnation} {second.status.message}")
        route_ok = (second.route.epoch >= 1 and len(second.route.keepers) >= 1
                    and second.assigned_keeper.process_id != "" and second.assigned_keeper.endpoint != "")
        self.check("Acquire carries route and assigned_keeper", route_ok)

    def placeholders(self) -> None:
        self.skip("Append", "Journal needs the real chrono_keeper image")
        self.skip("Read", "Replay needs the real chrono_player image")
        self.skip("Tail", "Replay needs the real chrono_player image")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--visor", default=os.environ.get("CHRONOLOG_SMOKE_VISOR", DEFAULT_VISOR))
    parser.add_argument("--ready-timeout", type=float, default=30.0)
    parser.add_argument("--rpc-timeout", type=float, default=10.0)
    args = parser.parse_args()

    out_dir = Path(tempfile.mkdtemp(prefix="chronolog-smoke-"))
    try:
        generate_stubs(out_dir)
        Smoke.stubs = out_dir
        smoke = Smoke(args.visor, args.rpc_timeout)
        if not smoke.wait_ready(args.ready_timeout):
            print(f"FAIL visor {args.visor} not reachable within {args.ready_timeout}s")
            return 1
        smoke.catalog_steps()
        smoke.placeholders()
    finally:
        shutil.rmtree(out_dir, ignore_errors=True)
    print("smoke: all active steps passed" if smoke.failures == 0 else f"smoke: {smoke.failures} failure(s)")
    return 0 if smoke.failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
