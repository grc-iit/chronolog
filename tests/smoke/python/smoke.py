#!/usr/bin/env python3
"""Exercise Catalog, Journal and Replay in the real compose stack."""

import argparse
import importlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent


def hlc_key(hlc):
    return hlc.physical_ns, hlc.logical


def event_key(event):
    return (*hlc_key(event.hlc), event.id.writer_id, event.id.incarnation, event.id.sequence)


class Smoke:
    def __init__(self, stubs, visor, timeout):
        import grpc

        sys.path.insert(0, str(stubs))
        self.pb = importlib.import_module("chronolog.v1.chronolog_pb2")
        self.rpc = importlib.import_module("chronolog.v1.chronolog_pb2_grpc")
        self.grpc = grpc
        self.timeout = timeout
        self.channels = []
        self.channel = self.connect(visor)
        self.catalog = self.rpc.CatalogStub(self.channel)

    def connect(self, endpoint):
        host, port = endpoint.rsplit(":", 1)
        if host in ("chrono-keeper", "chrono-player"):
            endpoint = f"127.0.0.1:{port}"
        channel = self.grpc.insecure_channel(endpoint)
        self.channels.append(channel)
        return channel

    def check(self, name, ok, detail=""):
        print(f"{'PASS' if ok else 'FAIL'} {name}{(' ' + detail) if detail else ''}", flush=True)
        if not ok:
            raise RuntimeError(name)

    def append(self, acquire, sequence, durability, floor=None):
        pb = self.pb
        item = pb.AppendItem(
            writer_id=acquire.writer_id, incarnation=acquire.incarnation, sequence=sequence,
            physical=pb.TimeReading(physical_ns=time.time_ns(), status=pb.CLOCK_STATUS_UNSYNCED),
            envelope=pb.Envelope(payload=f"event-{sequence}".encode()),
        )
        if floor is not None:
            item.causal_floor.CopyFrom(floor)
        response = self.journal.Append(pb.AppendRequest(
            story_id=acquire.story_id, epoch=acquire.route.epoch,
            items=[item], durability=durability, batch_id=sequence,
        ), timeout=self.timeout)
        if len(response.results) != 1 or response.batch_id != sequence:
            raise RuntimeError("Append response correlation or result count")
        return response.results[0]

    def read(self, request):
        events, completion = [], None
        for response in self.replay.Read(request, timeout=self.timeout):
            kind = response.WhichOneof("response")
            if completion is not None:
                raise RuntimeError("Replay data after Completion")
            if kind == "batch":
                events.extend(response.batch.events)
            elif kind == "completion":
                completion = response.completion
            else:
                raise RuntimeError("Replay response has no selected field")
        if completion is None:
            raise RuntimeError("Replay missing Completion")
        return events, completion

    def run(self, ready_timeout):
        pb = self.pb
        self.grpc.channel_ready_future(self.channel).result(timeout=ready_timeout)
        suffix = uuid.uuid4().hex[:8]
        chronicle, writer = f"smoke-{suffix}", f"writer-{suffix}"
        response = self.catalog.CreateChronicle(pb.CreateChronicleRequest(name=chronicle), timeout=self.timeout)
        self.check("CreateChronicle", response.status.code == 0 and response.chronicle.name == chronicle)
        response = self.catalog.CreateStory(pb.CreateStoryRequest(chronicle=chronicle, name="s1"), timeout=self.timeout)
        self.check("CreateStory", response.status.code == 0 and response.story.story_id != 0)
        story = response.story.story_id
        first = self.catalog.Acquire(pb.AcquireRequest(story_id=story, writer_identity=writer), timeout=self.timeout)
        self.check("Acquire incarnation 1", first.status.code == 0 and first.story_id == story
                   and first.writer_id != 0 and first.incarnation == 1 and first.route.epoch >= 1
                   and first.assigned_keeper.process_id != "" and first.assigned_keeper.endpoint != ""
                   and first.assigned_keeper in first.route.keepers and first.route.player != "")
        self.journal = self.rpc.JournalStub(self.connect(first.assigned_keeper.endpoint))
        self.replay = self.rpc.ReplayStub(self.connect(first.route.player))
        assigned = []
        floor = pb.Hlc()
        for sequence in range(1, 101):
            result = self.append(first, sequence, pb.DURABILITY_ACCEPTED, floor)
            if (result.status.code != 0 or result.achieved_durability != pb.DURABILITY_ACCEPTED
                    or hlc_key(result.assigned_hlc) <= hlc_key(floor)
                    or (result.id.story_id, result.id.writer_id, result.id.incarnation, result.id.sequence)
                    != (story, first.writer_id, 1, sequence)):
                self.check("Append 100 ACCEPTED", False, f"sequence={sequence} status={result.status}")
            assigned.append(result.assigned_hlc)
            floor = result.assigned_hlc
        self.check("Append 100 ACCEPTED with increasing HLC and causal_floor", True)
        durable = self.append(first, 101, pb.DURABILITY_DURABLE, floor)
        supported = durable.status.code == 0 and durable.achieved_durability == pb.DURABILITY_DURABLE
        self.check("Append DURABLE", supported or (durable.status.code == 12
                   and durable.achieved_durability == pb.DURABILITY_UNSPECIFIED),
                   "DURABLE" if supported else "UNIMPLEMENTED achieved=UNSPECIFIED")
        end = pb.Hlc(physical_ns=assigned[-1].physical_ns, logical=assigned[-1].logical + 1)
        request = pb.ReadRequest(story_id=story, hlc=pb.HlcRange(start=assigned[0], end=end))
        events, completion = self.read(request)
        expected_ids = [(story, first.writer_id, 1, sequence) for sequence in range(1, 101)]
        def correct_events(actual):
            return ([(e.id.story_id, e.id.writer_id, e.id.incarnation, e.id.sequence) for e in actual] == expected_ids
                    and [hlc_key(e.hlc) for e in actual] == [hlc_key(h) for h in assigned]
                    and [e.envelope.payload for e in actual] == [f"event-{n}".encode() for n in range(1, 101)])
        self.check("Read 100 complete", correct_events(events) and completion.complete
                   and completion.reason == pb.INCOMPLETE_REASON_UNSPECIFIED)
        idle = self.catalog.Acquire(pb.AcquireRequest(story_id=story, writer_identity=f"idle-{suffix}"), timeout=self.timeout)
        self.check("Acquire idle writer", idle.status.code == 0 and idle.writer_id != first.writer_id
                   and idle.incarnation == 1)
        events, completion = self.read(request)
        self.check("Read 100 complete with idle writer", correct_events(events) and completion.complete)
        future = pb.Hlc(physical_ns=completion.frontier.physical_ns + 60_000_000_000,
                        logical=completion.frontier.logical)
        _, lagging = self.read(pb.ReadRequest(story_id=story, hlc=pb.HlcRange(start=assigned[0], end=future)))
        self.check("Read above frontier", not lagging.complete and lagging.reason == pb.INCOMPLETE_REASON_LAGGING_WRITERS)
        _, physical = self.read(pb.ReadRequest(story_id=story,
                                              physical=pb.PhysicalRange(start_ns=0, end_ns=time.time_ns() + 1)))
        self.check("Physical Read", not physical.complete
                   and physical.reason == pb.INCOMPLETE_REASON_PHYSICAL_AXIS_UNBOUNDED)
        event50 = events[49]
        tail_request = pb.TailRequest(story_id=story)
        getattr(tail_request, "from").CopyFrom(pb.Position(hlc=event50.hlc, id=event50.id))
        tail = self.replay.Tail(tail_request, timeout=self.timeout)
        received = []
        try:
            for response in tail:
                if response.WhichOneof("response") != "batch":
                    raise RuntimeError("Tail ended before 50 events")
                received.extend(response.batch.events)
                if len(received) >= 50:
                    break
        finally:
            cancelled = tail.cancel()
        received = received[:50]
        self.check("Tail strictly after event 50 then cancel", len(received) == 50 and cancelled
                   and [event_key(e) for e in received] == [event_key(e) for e in events[50:]]
                   and all(event_key(e) > event_key(event50) for e in received))
        released = self.catalog.Release(pb.ReleaseRequest(story_id=story, writer_id=first.writer_id,
                                                           incarnation=first.incarnation), timeout=self.timeout)
        self.check("Release fenced", released.status.code == 0 and released.fenced)
        rejected = self.append(first, 102 if supported else 101, pb.DURABILITY_ACCEPTED, floor)
        self.check("Released incarnation append", rejected.status.code == 9
                   and rejected.achieved_durability == pb.DURABILITY_UNSPECIFIED)
        second = self.catalog.Acquire(pb.AcquireRequest(story_id=story, writer_identity=writer), timeout=self.timeout)
        self.check("Acquire incarnation 2", second.status.code == 0 and second.incarnation == 2
                   and second.writer_id == first.writer_id)
        print("SMOKE PASS", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--visor", default=os.environ.get("CHRONOLOG_SMOKE_VISOR", "127.0.0.1:50051"))
    parser.add_argument("--ready-timeout", type=float, default=30.0)
    parser.add_argument("--rpc-timeout", type=float, default=10.0)
    args = parser.parse_args()
    out_dir = Path(tempfile.mkdtemp(prefix="chronolog-smoke-"))
    smoke = None
    try:
        subprocess.run([str(HERE / "gen_stubs.sh"), str(out_dir)], check=True, timeout=30,
                       env={**os.environ, "PYTHON": sys.executable}, stdout=subprocess.DEVNULL)
        smoke = Smoke(out_dir, args.visor, args.rpc_timeout)
        smoke.run(args.ready_timeout)
        return 0
    except Exception as error:
        print(f"FAIL {error}", flush=True)
        return 1
    finally:
        if smoke is not None:
            for channel in smoke.channels:
                channel.close()
        shutil.rmtree(out_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
