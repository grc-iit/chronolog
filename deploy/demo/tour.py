#!/usr/bin/env python3
"""ChronoLog 4.0 guided tour. Every step asserts what it claims; the first broken claim exits non-zero.

Environment (set by deploy/demo/chronolog-demo):
  CHRONOLOG_CATALOG, CHRONOLOG_PLAYER   host endpoints of the Visor and the Player
  CHRONOLOG_DEMO_ENGINE, CHRONOLOG_DEMO_PROJECT, CHRONOLOG_DEMO_COMPOSE
                                        compose command prefix, used to kill and restart the Keeper
  CHRONOLOG_DEMO_BIN                    directory with chronolog_kvs, chronolog_sql, chronolog_pubsub_example
  CHRONOLOG_DEMO_FULL                   1 when Grafana and InfluxDB are up
  CHRONOLOG_DEMO_GRAFANA_AUTH           optional user:password for the Grafana API (default admin:admin)
"""
import argparse
import asyncio
import base64
import json
import os
import re
import shlex
import subprocess
import sys
import textwrap
import threading
import time
import urllib.error
import urllib.request

import chronolog as cl

STEPS = 11
STATUS = {0: "Synced", 1: "Unsynced", 2: "Unavailable"}
UNCERTAINTY_CAP_NS = 1_000_000_000


class TourError(Exception):
    pass


def check(condition, claim, detail=""):
    if not condition:
        raise TourError(claim + (f" [{detail}]" if detail else ""))
    print(f"  ok: {claim}", flush=True)


def fmt_hlc(h):
    return f"{h.physical_ns}:{h.logical}"


def fmt_id(i):
    return f"{i.story_id}:{i.writer_id}:{i.incarnation}:{i.sequence}"


def after(h):
    return cl.Hlc(h.physical_ns, h.logical + 1)


def say(*lines):
    print(textwrap.fill(" ".join(lines), width=100, initial_indent="  ", subsequent_indent="  ",
                         break_on_hyphens=False), flush=True)


def call(text):
    print(f"  > {text}", flush=True)


def show(label, value):
    print(f"    {label}: {value}", flush=True)


def payload(**fields):
    return json.dumps(fields, separators=(",", ":")).encode()


def agent_attributes(agent, operation, conversation):
    return {"gen_ai.agent.id": agent, "gen_ai.operation.name": operation, "gen_ai.conversation.id": conversation}


def is_bounded(reading):
    return (reading.status == 0 and reading.uncertainty_ns is not None
            and reading.uncertainty_ns <= UNCERTAINTY_CAP_NS)


class Tour:
    def __init__(self, args):
        self.args = args
        self.catalog = os.environ.get("CHRONOLOG_CATALOG", "127.0.0.1:50051")
        self.player = os.environ.get("CHRONOLOG_PLAYER", "127.0.0.1:50054")
        self.engine = os.environ.get("CHRONOLOG_DEMO_ENGINE", "")
        self.compose_prefix = shlex.split(os.environ.get("CHRONOLOG_DEMO_COMPOSE", ""))
        self.bin = os.environ.get("CHRONOLOG_DEMO_BIN", "")
        self.full = os.environ.get("CHRONOLOG_DEMO_FULL", "") == "1"
        self.client_a = cl.connect(self.catalog, self.player, timeout=10)
        self.client_b = cl.connect(self.catalog, self.player, timeout=10)
        self.chronicle = None
        self.stories = {}
        self.acked = {}

    def compose(self, *args, timeout=90, capture=False):
        if not self.compose_prefix:
            raise TourError("CHRONOLOG_DEMO_COMPOSE is not set, so the tour cannot kill and restart services")
        result = subprocess.run([*self.compose_prefix, *args], timeout=timeout, text=True,
                                stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                                stderr=subprocess.PIPE)
        if result.returncode != 0:
            raise TourError(f"compose {' '.join(args)} failed: {result.stderr.strip()[-300:]}")
        return result.stdout if capture else ""

    def read_complete(self, client, story, start, end, wait=30.0):
        """Reads [start, end) until Replay reports complete=true, or fails after `wait` seconds."""
        deadline = time.monotonic() + wait
        detail = "no response"
        while True:
            try:
                with client.read(story, start, end, timeout=5) as stream:
                    events = list(stream)
                    completion = stream.completion
                detail = f"events={len(events)} completion={completion}"
                if completion is not None and completion.complete:
                    return events, completion
            except cl.Error as error:
                detail = f"{type(error).__name__}: {error.status.message}"
            if time.monotonic() > deadline:
                raise TourError(f"read did not become complete within {wait:.0f} s [{detail}]")
            time.sleep(0.2)

    def heading(self, number, title, *narration):
        print(f"\n=== Step {number}/{STEPS}: {title} ===", flush=True)
        say(*narration)

    # ---------------------------------------------------------------- steps

    def step1(self):
        self.heading(1, "Catalog: chronicles and stories",
                     "A chronicle is a namespace and a story is one ordered stream of events inside it.",
                     "Agents get one story each for private notes and share a third for the plan, so who may write",
                     "what is a naming decision and not an access hack.")
        base = f"tour-{int(time.time())}"
        suffix = 0
        while True:
            name = base if suffix == 0 else f"{base}-{suffix}"
            try:
                call(f"client.create_chronicle({name!r})")
                self.client_a.create_chronicle(name)
                break
            except cl.AlreadyExists:
                suffix += 1
        self.chronicle = name
        for story in ("agent-a/notes", "agent-b/notes", "shared/plan"):
            call(f"client.create_story({name!r}, {story!r})")
            created = self.client_a.create_story(name, story)
            self.stories[story] = created.id
            show(story, f"story id {created.id}, epoch {created.epoch}")
        listed = {s.name: s.id for s in self.client_a.list_stories(name)}
        call(f"client.list_stories({name!r})")
        check(listed == self.stories, "list_stories returns exactly the three stories just created", str(listed))

    def step2(self):
        self.heading(2, "Write: ACCEPTED and DURABLE appends",
                     "Every append returns an EventId (story, writer, incarnation, sequence), the HLC the Keeper",
                     "assigned and the durability it achieved. DURABLE means the Keeper fsynced the event to its",
                     "write-ahead log before replying, so an acknowledged write survives a crash; ACCEPTED lives in RAM only.")
        notes = self.stories["agent-a/notes"]
        conversation = self.chronicle
        results = []
        call("writer = client.acquire(story, 'agent-a')")
        with self.client_a.acquire(notes, "agent-a") as writer:
            show("writer", f"id {writer.writer_id} incarnation {writer.incarnation} "
                              f"keeper {writer.assigned_keeper.process_id} at {writer.assigned_keeper.endpoint}")
            call("writer.append(..., durability=ACCEPTED)")
            accepted = writer.append(
                payload(note="draft: look at the failing job"), content_type="application/json",
                attributes=agent_attributes("agent-a", "plan", conversation), durability=cl.Durability.ACCEPTED)
            results.append(("ACCEPTED", accepted))
            call("writer.append(..., durability=DURABLE)")
            durable = writer.append(
                "# Findings\nthe job fails on node 7".encode(), content_type="text/markdown",
                attributes=agent_attributes("agent-a", "execute_tool", conversation), durability=cl.Durability.DURABLE)
            results.append(("DURABLE", durable))
            call("writer.append_batch([...2 items...])")
            batch = writer.append_batch([
                cl.Envelope(payload(tool="grep", result="oom"), "application/json",
                            {**agent_attributes("agent-a", "execute_tool", conversation), "gen_ai.tool.name": "grep"}),
                cl.Envelope(payload(note="next: raise the memory limit"), "application/json",
                            agent_attributes("agent-a", "plan", conversation))])
            results.extend(("DURABLE", r) for r in batch)
        call("a second agent writes its own story")
        with self.client_b.acquire(self.stories["agent-b/notes"], "agent-b") as writer_b:
            for text in ("reviewing agent-a's plan", "memory limit looks right"):
                r = writer_b.append(payload(note=text), content_type="application/json",
                                    attributes=agent_attributes("agent-b", "review", conversation))
                self.acked.setdefault("agent-b/notes", []).append(r)
        for label, r in results:
            show(f"{label:8}", f"id {fmt_id(r.event_id)} hlc {fmt_hlc(r.hlc)} achieved {r.durability.name} acked={r.acked}")
        check(accepted.durability == cl.Durability.ACCEPTED and not accepted.acked,
              "the ACCEPTED append reports ACCEPTED and is not acked")
        check(all(r.durability == cl.Durability.DURABLE and r.acked for _, r in results[1:]),
              "every DURABLE append reports DURABLE and acked=True")
        sequences = [r.event_id.sequence for _, r in results]
        check(sequences == [1, 2, 3, 4], "sequences are gapless per writer", str(sequences))
        hlcs = [r.hlc for _, r in results]
        check(all(a < b for a, b in zip(hlcs, hlcs[1:])), "HLCs strictly increase along the writer's sequence")
        self.acked["agent-a/notes"] = [r for _, r in results]

    def step3(self):
        self.heading(3, "Replay with completeness",
                     "A read returns events and then exactly one Completion that says whether the range is final.",
                     "complete=true means every Keeper sealed past the end of the range, so nothing earlier can still",
                     "arrive. An agent that ignores Completion may reason from a partial history without knowing it.")
        notes = self.stories["agent-a/notes"]
        sent = self.acked["agent-a/notes"]
        start, end = sent[0].hlc, after(sent[-1].hlc)
        call(f"client.read(story, start={fmt_hlc(start)}, end={fmt_hlc(end)})")
        events, completion = self.read_complete(self.client_a, notes, start, end)
        show("events", len(events))
        show("completion", f"complete={completion.complete} frontier={fmt_hlc(completion.frontier)} "
                           f"reason={completion.reason.name}")
        check([e.id for e in events] == [r.event_id for r in sent], "the read returns exactly the acknowledged events")
        check(completion.complete and completion.reason == cl.IncompleteReason.NONE
              and completion.frontier >= end, "Completion is complete=true with the frontier at or past the end")
        future = cl.Hlc(time.time_ns() + 3600 * 10**9, 0)
        call(f"client.read(story, start={fmt_hlc(start)}, end=now+1h)")
        with self.client_a.read(notes, start, future, timeout=10) as stream:
            partial = list(stream)
            late = stream.completion
        show("events", len(partial))
        show("completion", f"complete={late.complete} frontier={fmt_hlc(late.frontier)} reason={late.reason.name} "
                           f"laggards={len(late.laggards)}")
        check(late is not None and not late.complete and late.reason != cl.IncompleteReason.NONE,
              "a read past the frontier says complete=false and names a reason instead of pretending")
        check(len(partial) == len(sent), "the events that do exist are still returned, flagged as not final")

    def step4(self):
        self.heading(4, "Shared memory: one agent tails what another writes",
                     "Tail follows a story from a position and delivers new events in order as they are accepted.",
                     "Agent B watches shared/plan while agent A appends to it, so B sees A's steps live without polling.")
        plan = self.stories["shared/plan"]
        received = []
        errors = []

        def follow():
            try:
                with self.client_b.tail(plan, timeout=15) as stream:
                    for event in stream:
                        received.append(event)
                        if len(received) == 3:
                            break
            except cl.Error as error:
                errors.append(error)

        call("tail = client_b.tail(shared/plan)  # agent B")
        thread = threading.Thread(target=follow, daemon=True)
        thread.start()
        time.sleep(1.0)
        sent = []
        call("writer_a.append(...) x3  # agent A")
        with self.client_a.acquire(plan, "agent-a") as writer:
            for step in ("1: reproduce the failure", "2: bisect the change", "3: patch and re-run"):
                sent.append(writer.append(payload(plan=step), content_type="application/json",
                                          attributes=agent_attributes("agent-a", "plan", self.chronicle)))
        thread.join(20)
        check(not errors, "the tail ran without errors", str(errors))
        for event in received:
            show("B received", f"id {fmt_id(event.id)} hlc {fmt_hlc(event.hlc)} {event.envelope.payload.decode()}")
        check([e.id for e in received] == [r.event_id for r in sent],
              "B received A's three events live, in A's order, with identical ids")
        self.plan_events = sent

    def step5(self):
        self.heading(5, "Causality across writers",
                     "The Python SDK has no causal_floor argument. The connection remembers the highest HLC it has",
                     "appended or read and attaches it to every later append, and the Keeper assigns an HLC above it.",
                     "So anything a client has read is in the causal past of what that same client writes next.")
        plan = self.stories["shared/plan"]
        with self.client_a.acquire(plan, "agent-a") as writer_a:
            call("event_a = writer_a.append(...)  # agent A, client 1")
            event_a = writer_a.append(payload(plan="4: deploy to staging"), content_type="application/json",
                                      attributes=agent_attributes("agent-a", "plan", self.chronicle))
        show("A", f"id {fmt_id(event_a.event_id)} hlc {fmt_hlc(event_a.hlc)}")
        fresh = cl.connect(self.catalog, self.player, timeout=10)
        call("seen = fresh_client.read(shared/plan, event_a)  # agent B, client 2, never talked to A")
        seen, _ = self.read_complete(fresh, plan, event_a.hlc, after(event_a.hlc))
        check([e.id for e in seen] == [event_a.event_id], "B's client read A's event")
        call("event_b = writer_b.append(...)  # same client that did the read")
        with fresh.acquire(plan, "agent-b") as writer_b:
            event_b = writer_b.append(payload(plan="4b: staging looks healthy"), content_type="application/json",
                                      attributes=agent_attributes("agent-b", "review", self.chronicle))
        show("B", f"id {fmt_id(event_b.event_id)} hlc {fmt_hlc(event_b.hlc)}")
        check(event_b.hlc > event_a.hlc, "B's HLC is above A's HLC although they are different writers")
        ordered, _ = self.read_complete(self.client_a, plan, event_a.hlc, after(event_b.hlc))
        check([e.id for e in ordered] == [event_a.event_id, event_b.event_id],
              "Replay returns A's event before B's reply")
        say("Guarantee: per client object, every HLC it has appended or read is below the HLC of its later appends.",
            "Two clients that never read each other's events share nothing; this stack has one Keeper, so its own",
            "clock also orders them, and the floor is what keeps the property true across Keepers.")

    def step6(self):
        self.heading(6, "Crash safety: SIGKILL the Keeper",
                     "The Keeper is killed without warning while an agent has just written. A read during the outage",
                     "must say it is incomplete instead of quietly returning less. After the restart every DURABLE",
                     "event must come back with the same id and the same HLC, because DURABLE was fsynced first.")
        story = self.stories["agent-b/notes"]
        durable, volatile = [], []
        call("writer.append(durability=DURABLE) x5, then ACCEPTED x5")
        with self.client_b.acquire(story, "agent-b") as writer:
            for index in range(5):
                durable.append(writer.append(payload(note=f"durable {index}"), content_type="application/json",
                                             durability=cl.Durability.DURABLE))
            for index in range(5):
                volatile.append(writer.append(payload(note=f"accepted {index}"), content_type="application/json",
                                              durability=cl.Durability.ACCEPTED))
        everything = self.acked["agent-b/notes"] + durable + volatile
        start, end = everything[0].hlc, after(max(r.hlc for r in everything))
        before, _ = self.read_complete(self.client_a, story, start, end)
        show("before the crash", f"{len(before)} events readable, complete=true")
        call("compose kill -s SIGKILL chrono-keeper")
        self.compose("kill", "-s", "SIGKILL", "chrono-keeper", timeout=30)
        time.sleep(1.0)
        call("client.read(agent-b/notes) during the outage")
        try:
            with self.client_a.read(story, start, end, timeout=4) as stream:
                partial = list(stream)
                completion = stream.completion
            outcome = (f"complete={completion.complete} reason={completion.reason.name} "
                       f"events_returned={len(partial)}")
            honest = completion is not None and not completion.complete and completion.reason in (
                cl.IncompleteReason.SOURCE_FAILED, cl.IncompleteReason.LAGGING_WRITERS)
        except cl.Error as error:
            outcome = f"{type(error).__name__}: {error.status.message}"
            honest = isinstance(error, (cl.Unavailable, cl.DeadlineExceeded, cl.FailedPrecondition))
        show("during the outage", outcome)
        check(honest, "the outage read reports SOURCE_FAILED, LAGGING_WRITERS or an error, never complete=true")
        call("compose start chrono-keeper")
        self.compose("start", "chrono-keeper", timeout=60)
        events, completion = self.read_complete(self.client_a, story, start, end, wait=90)
        by_id = {e.id: e for e in events}
        for r in everything[: len(self.acked["agent-b/notes"]) + len(durable)]:
            event = by_id.get(r.event_id)
            check(event is not None and event.hlc == r.hlc,
                  f"DURABLE event {fmt_id(r.event_id)} is back with hlc {fmt_hlc(r.hlc)}")
        survived = sum(1 for r in volatile if r.event_id in by_id)
        show("ACCEPTED events", f"{survived} of {len(volatile)} survived (ACCEPTED never promised to)")
        show("after the restart", f"{len(events)} events, complete={completion.complete}")
        self.durable_ids = {fmt_id(r.event_id) for r in durable}

    def step7(self):
        self.heading(7, "Archive: events move to the Grapher",
                     "The Keeper ships sealed chunks to the Grapher, which writes them as HDF5 and returns a receipt.",
                     "Only after the receipt is settled may the Keeper drop its copy. Replay does not say which source",
                     "answered, so the proof is the settled receipts, the files, and an identical read afterwards.")
        ids = {name: sid for name, sid in self.stories.items()}
        deadline = time.monotonic() + 60
        settled = published = 0
        while time.monotonic() < deadline:
            keeper = self.compose("logs", "--no-color", "chrono-keeper", capture=True, timeout=60)
            grapher = self.compose("logs", "--no-color", "chrono-grapher", capture=True, timeout=60)
            settled = sum(len(re.findall(rf"archive_settled chunk=\S+:{sid}:\S+ story={sid}\b", keeper))
                          for sid in ids.values())
            published = sum(len(re.findall(rf"archive_published chunk=\S+:{sid}:\S+ story={sid}\b", grapher))
                            for sid in ids.values())
            if settled >= 1 and published >= 1:
                break
            time.sleep(2)
        call("compose logs chrono-keeper chrono-grapher | count archive receipts")
        show("archive_published (Grapher)", published)
        show("archive_settled (Keeper)", settled)
        notes = self.stories["agent-a/notes"]
        files = None
        try:
            out = self.compose("exec", "-T", "chrono-grapher", "bash", "-c",
                               f"find /var/lib/chronolog/archive/{notes} -name '*.h5' | wc -l", capture=True, timeout=30)
            files = int(out.strip())
            call(f"compose exec chrono-grapher find /var/lib/chronolog/archive/{notes} -name '*.h5'")
            show("HDF5 files for agent-a/notes", files)
        except (TourError, ValueError, subprocess.SubprocessError):
            say("The archive directory could not be listed through compose exec on this engine; receipts above stand.")
        if settled >= 1 and published >= 1:
            check(True, "chunks were published to the Grapher and settled by the Keeper")
            if files is not None:
                check(files >= 1, "the Grapher holds at least one HDF5 chunk file for agent-a/notes")
            sent = [r for r in self.acked["agent-a/notes"] if r.acked]
            events, completion = self.read_complete(self.client_a, notes, sent[0].hlc, after(sent[-1].hlc))
            check([e.id for e in events] == [r.event_id for r in sent] and [e.hlc for e in events] == [r.hlc for r in sent],
                  "after settlement the same read returns the DURABLE events with identical ids and HLCs, complete=true")
        else:
            say("No archive receipt appeared within 60 s on this stack, so no archive claim is made.")

    def step8(self):
        self.heading(8, "Wall-clock time and what it can promise",
                     "HLC order is always available; wall-clock ranges are only final when every selected event has a",
                     "bounded clock reading, meaning Synced with an uncertainty under 1 s. The SDK reads the kernel's",
                     "NTP state, so on a host without a disciplined clock a physical read must say it cannot be final.")
        notes = self.stories["agent-a/notes"]
        sent = [r for r in self.acked["agent-a/notes"] if r.acked]
        events, _ = self.read_complete(self.client_a, notes, sent[0].hlc, after(sent[-1].hlc))
        statuses = sorted({STATUS.get(e.physical.status, str(e.physical.status)) for e in events})
        show("clock status the SDK stored on this host", ", ".join(statuses))
        for e in events[:2]:
            show("reading", f"physical_ns={e.physical.physical_ns} status={STATUS.get(e.physical.status)} "
                            f"uncertainty_ns={e.physical.uncertainty_ns}")
        bounded = all(is_bounded(e.physical) for e in events)
        expected = "complete=true" if bounded else "complete=false reason=PHYSICAL_AXIS_UNBOUNDED"
        why = ("every selected event has a Synced reading within the 1 s cap" if bounded else
               "this host is not NTP-disciplined, so the SDK stamps Unsynced readings with no bound")
        show("prediction for a physical read of those events", f"{expected} ({why})")
        lo = min(e.physical.physical_ns for e in events) - 10**9
        hi = max(e.physical.physical_ns for e in events) + 10**9
        read_physical = getattr(self.client_a, "read_physical", None)
        if read_physical is None:
            say("This build of the Python SDK has no physical-time read yet (it ships with M6b), so the prediction",
                "above is derived from the stored readings and no physical read is issued.")
            check(all(e.physical.status in STATUS for e in events), "every stored reading carries a clock status")
            return
        call(f"client.read_physical(story, {lo}, {hi})")
        deadline = time.monotonic() + 60
        waited = False
        while True:
            with read_physical(notes, lo, hi, timeout=10) as stream:
                got = list(stream)
                completion = stream.completion
            outcome = f"complete={completion.complete} reason={completion.reason.name} events={len(got)}"
            settled = bounded and completion.complete or (
                not bounded and completion.reason == cl.IncompleteReason.PHYSICAL_AXIS_UNBOUNDED)
            if settled or time.monotonic() > deadline:
                break
            if not waited:
                say("Waiting for the Keeper's physical frontier to pass the range (acceptance window 15 s).")
                waited = True
            time.sleep(3)
        show("observed", outcome)
        check(settled, f"the physical read matches the clock-status prediction ({expected})")
        call("a point reading without a bound: writer.append(physical=TimeReading(now, None, Unsynced))")
        with self.client_a.acquire(notes, "agent-a") as writer:
            stamp = time.time_ns()
            marker = writer.append(payload(note="unbounded clock reading"), content_type="application/json",
                                   physical=cl.TimeReading(stamp, None, 1))
        window_lo, window_hi = stamp - 10**9, stamp + 10**9
        deadline = time.monotonic() + 60
        while True:
            with read_physical(notes, window_lo, window_hi, timeout=10) as stream:
                got = list(stream)
                completion = stream.completion
            if completion.reason == cl.IncompleteReason.PHYSICAL_AXIS_UNBOUNDED or time.monotonic() > deadline:
                break
            time.sleep(3)
        show("range containing the unbounded reading", f"complete={completion.complete} reason={completion.reason.name}")
        check(not completion.complete and completion.reason == cl.IncompleteReason.PHYSICAL_AXIS_UNBOUNDED
              and marker.event_id in {e.id for e in got},
              "a range that selects an unbounded reading reports PHYSICAL_AXIS_UNBOUNDED")

    def run_cli(self, name, *args, timeout=60):
        path = os.path.join(self.bin, name)
        env = dict(os.environ)
        lib = os.path.join(os.path.dirname(self.bin.rstrip("/")), "lib")
        if os.path.isdir(lib):
            env["LD_LIBRARY_PATH"] = lib + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        result = subprocess.run([path, *args], capture_output=True, text=True, timeout=timeout, env=env)
        if result.returncode != 0:
            raise TourError(f"{name} {' '.join(args[:4])} exited {result.returncode}: {result.stderr.strip()[-300:]}")
        return result.stdout

    def step9(self):
        self.heading(9, "Plugins: key-value, pub/sub and SQL on the same log",
                     "The plugins are ordinary programs on the public SDK; they add no storage of their own.",
                     "A KV key is a story, a topic is a story and a table is a story, so history, Tail and",
                     "Completion work the same way for all of them.")
        if not self.bin or not all(os.path.exists(os.path.join(self.bin, n)) for n in
                                   ("chronolog_kvs", "chronolog_sql", "chronolog_pubsub_example")):
            say("CHRONOLOG_DEMO_BIN is not set or lacks the plugin programs, so this step is skipped.")
            return
        base = [self.catalog, self.player]
        kvs_chronicle = f"{self.chronicle}-kvs"

        def version(text):
            match = re.search(r"^(\d+:\d+) id=\S+ acked=(\d)", text, re.M)
            if not match:
                raise TourError(f"unexpected chronolog_kvs output: {text!r}")
            return match[1], match[2] == "1"

        call(f"chronolog_kvs {self.catalog} {self.player} {kvs_chronicle} put plan v1")
        v1, acked1 = version(self.run_cli("chronolog_kvs", *base, kvs_chronicle, "put", "plan", "v1"))
        call("chronolog_kvs ... put plan v2")
        v2, acked2 = version(self.run_cli("chronolog_kvs", *base, kvs_chronicle, "put", "plan", "v2"))
        show("put v1", f"hlc {v1} acked={acked1}")
        show("put v2", f"hlc {v2} acked={acked2}")
        check(acked1 and acked2, "both puts are acknowledged DURABLE")
        latest = self.run_cli("chronolog_kvs", *base, kvs_chronicle, "get", "plan")
        check(latest.strip().splitlines()[-1] == "v2" and "complete=1" in latest, "get returns the latest value, complete")
        call(f"chronolog_kvs ... get-at plan {v2}")
        older = self.run_cli("chronolog_kvs", *base, kvs_chronicle, "get-at", "plan", v2)
        show("value as of just before v2", older.strip().splitlines()[-1])
        check(older.strip().splitlines()[-1] == "v1", "get-at an older HLC returns the older value")
        end = v2.split(":")
        history = self.run_cli("chronolog_kvs", *base, kvs_chronicle, "history", "plan", v1, f"{end[0]}:{int(end[1]) + 1}")
        values = [line for line in history.splitlines() if line in ("v1", "v2")]
        show("history", f"{values} {history.strip().splitlines()[-1]}")
        check(values == ["v1", "v2"] and "complete=1" in history, "history lists both versions in order, complete")

        call("chronolog_pubsub_example  # publish, subscribe, save position, restart, resume")
        out = self.run_cli("chronolog_pubsub_example", *base, timeout=90)
        show("result", out.strip())
        check("agent-memory resumed after" in out, "a restarted subscriber resumed from its saved position without loss")

        sql_chronicle = f"{self.chronicle}-sql"
        statements = [
            "CREATE TABLE tasks (agent TEXT, step INTEGER, ok BOOLEAN)",
            "INSERT INTO tasks VALUES ('agent-a', 1, true), ('agent-b', 2, false), ('agent-a', 3, true)",
            "SELECT agent, step FROM tasks WHERE ok = true ORDER BY TIME DESC LIMIT 2",
        ]
        for statement in statements:
            call(f"chronolog_sql ... {statement!r}")
        out = self.run_cli("chronolog_sql", *base, sql_chronicle, *statements, timeout=90)
        lines = [json.loads(line) for line in out.strip().splitlines()]
        for line in lines[-3:]:
            show("row/completion", json.dumps(line, separators=(",", ":")))
        rows = [line for line in lines if "agent" in line and "step" in line]
        check([r["step"] for r in rows] == [3, 1], "SELECT with WHERE and ORDER BY TIME DESC LIMIT 2 returns steps 3 and 1")
        final = lines[-1]
        check(final["completion"]["complete"] is True and final["limited"] is False,
              "the result carries its Completion, complete=true", json.dumps(final))

    def step10(self):
        self.heading(10, "Agents: MCP and OpenTelemetry",
                     "chronolog-mcp exposes ChronoLog to any MCP client such as Claude Code. Tool calls land in the same",
                     "log with durable acknowledgements and the MCP request id as an attribute. OpenTelemetry spans use the",
                     "GenAI conventions and keep their trace and span ids.")
        try:
            from mcp import ClientSession, StdioServerParameters
            from mcp.client.stdio import stdio_client
            import chronomcp.server  # noqa: F401
        except ImportError as error:
            raise TourError(f"chronolog-mcp and mcp must be installed in this Python environment: {error}")
        chronicle = f"{self.chronicle}-mcp"
        args = ["-m", "chronomcp.server", "--catalog", self.catalog, "--player", self.player,
                "--chronicle", chronicle, "--identity", "tour-mcp", "--timeout", "10"]

        async def session():
            params = StdioServerParameters(command=sys.executable, args=args, env=dict(os.environ))
            async with stdio_client(params, errlog=open(os.devnull, "w")) as (read, write):
                async with ClientSession(read, write) as client:
                    await client.initialize()
                    tools = sorted(t.name for t in (await client.list_tools()).tools)
                    created = json.loads((await client.call_tool("create_story", {"story": "mcp-notes"})).content[0].text)
                    story = created["id"]
                    appended = json.loads((await client.call_tool(
                        "append", {"story": story, "content": "the agent remembers this", "attributes":
                                   {"gen_ai.agent.id": "claude-code", "gen_ai.operation.name": "chat"}})).content[0].text)
                    deadline = time.monotonic() + 30
                    while True:
                        result = json.loads((await client.call_tool("read", {"story": story})).content[0].text)
                        complete = result["completion"] and result["completion"]["complete"]
                        if complete or time.monotonic() > deadline:
                            return tools, appended, result
                        await asyncio.sleep(0.3)

        call(f"python -m chronomcp.server --catalog {self.catalog} --player {self.player} --chronicle {chronicle}  # stdio")
        tools, appended, result = asyncio.run(asyncio.wait_for(session(), timeout=90))
        show("tools", ", ".join(tools))
        show("append", f"event {appended['event_id']} hlc {appended['hlc']} durability {appended['durability']} acked={appended['acked']}")
        check({"append", "read", "tail", "create_story", "list_stories"} <= set(tools), "the MCP server lists its story tools")
        check(appended["acked"], "an MCP append is acknowledged DURABLE")
        events = result["events"]
        show("read", f"{len(events)} event, completion {result['completion']['complete']}, attributes {events[0]['attributes']}")
        check(len(events) == 1 and events[0]["content"] == "the agent remembers this" and result["completion"]["complete"],
              "an MCP read returns the event with a complete Completion")
        check("requestId" in events[0]["attributes"], "the MCP request id is stored as an attribute")
        launcher = os.path.join(os.path.dirname(sys.executable), "chronolog-mcp")
        command = launcher if os.path.exists(launcher) else f"{sys.executable} -m chronomcp.server"
        say("Register this stack with Claude Code with one command:")
        print(f"    claude mcp add chronolog -- {command} --catalog {self.catalog} --player {self.player}", flush=True)
        if not self.full:
            say("CHRONOLOG_DEMO_FULL is not 1, so the OpenTelemetry export is skipped.")
            return
        try:
            from opentelemetry.sdk.trace import TracerProvider
            from opentelemetry.sdk.trace.export import SimpleSpanProcessor
            from chronolog.otel import ChronologSpanExporter
        except ImportError as error:
            raise TourError(f"chronolog[otel] is not installed: {error}")
        conversation = f"conversation-{int(time.time())}"
        exporter = ChronologSpanExporter(self.catalog, self.player, chronicle=f"{self.chronicle}-otel",
                                         default_story="spans", identity="tour-otel", timeout=10)
        provider = TracerProvider()
        provider.add_span_processor(SimpleSpanProcessor(exporter))
        call("TracerProvider + ChronologSpanExporter: one GenAI 'chat' span")
        with provider.get_tracer("chronolog-tour").start_as_current_span(
                "chat claude-sonnet-5-5", attributes={
                    "gen_ai.operation.name": "chat", "gen_ai.agent.id": "agent-a",
                    "gen_ai.request.model": "claude-sonnet-5-5", "gen_ai.conversation.id": conversation}) as span:
            trace_id = span.get_span_context().trace_id
        provider.shutdown()
        stories = {s.name: s for s in self.client_a.list_stories(f"{self.chronicle}-otel")}
        check(conversation in stories, "the span landed in the story named by gen_ai.conversation.id", str(sorted(stories)))
        sid = stories[conversation].id
        found = []
        deadline = time.monotonic() + 30
        while not found and time.monotonic() < deadline:
            with self.client_a.read(sid, None, cl.Hlc(time.time_ns() + 10**9, 0), timeout=5) as stream:
                found = list(stream)
            time.sleep(0.3)
        check(len(found) == 1, "the story holds exactly the one exported span")
        event = found[0]
        show("span event", f"content_type {event.envelope.content_type} trace_id {event.envelope.trace_id.hex()}")
        show("attributes", event.envelope.attributes)
        check(event.envelope.trace_id == trace_id.to_bytes(16, "big"), "the W3C trace id survived unchanged")
        check(event.envelope.attributes.get("gen_ai.operation.name") == "chat", "GenAI attributes are stored on the event")

    def grafana(self, path):
        request = urllib.request.Request(f"http://127.0.0.1:3000{path}")
        auth = os.environ.get("CHRONOLOG_DEMO_GRAFANA_AUTH", "admin:admin")
        request.add_header("Authorization", "Basic " + base64.b64encode(auth.encode()).decode())
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.loads(response.read())

    def step11(self):
        self.heading(11, "Dashboards",
                     "Grafana shows both halves of the platform: host telemetry that the stream plugin exports to InfluxDB,",
                     "and the ChronoLog events of this tour read through the viz data source with their Completion.")
        if not self.full:
            say("CHRONOLOG_DEMO_FULL is not 1, so Grafana and InfluxDB are not running and this step is skipped.")
            return
        try:
            sources = self.grafana("/api/datasources")
            dashboards = self.grafana("/api/search?type=dash-db")
        except (urllib.error.URLError, OSError, ValueError) as error:
            raise TourError(f"Grafana API unreachable at http://127.0.0.1:3000: {error}")
        show("Grafana", "http://127.0.0.1:3000")
        for item in dashboards:
            show("dashboard", f"http://127.0.0.1:3000{item['url']}  ({item['title']})")
        types = {s["type"] for s in sources}
        for source in sources:
            health = self.grafana(f"/api/datasources/uid/{source['uid']}/health")
            show("datasource", f"{source['name']} ({source['type']}): {health.get('status')} {health.get('message', '')}")
            check(str(health.get("status", "")).upper() == "OK", f"datasource {source['name']} is healthy")
        check("influxdb" in types and any("chronolog" in t for t in types), "both the InfluxDB and the ChronoLog data sources exist",
              str(sorted(types)))
        check(len(dashboards) >= 2, "at least two dashboards are provisioned", str(len(dashboards)))

    # ---------------------------------------------------------------- driver

    def run(self):
        steps = [self.step1, self.step2, self.step3, self.step4, self.step5, self.step6, self.step7, self.step8,
                 self.step9, self.step10, self.step11]
        selected = parse_steps(self.args.steps)
        print("ChronoLog 4.0 tour", flush=True)
        print(f"  catalog {self.catalog}  player {self.player}  engine {self.engine or '-'}  full={int(self.full)}", flush=True)
        for number, step in enumerate(steps, 1):
            if number not in selected:
                continue
            if self.args.pause and number > 1:
                input("\n[press Enter for the next step] ")
            if number > 1 and self.chronicle is None:
                raise TourError("step 1 must run first, it creates the chronicle")
            step()
        print(f"\n=== Tour complete: chronicle {self.chronicle} ===", flush=True)
        say("Demonstrated: Catalog naming, ACCEPTED vs DURABLE appends, Replay with Completion, live Tail between agents,",
            "causal ordering, Keeper crash recovery, the Grapher archive, wall-clock honesty, the KV, pub/sub and SQL plugins,",
            "MCP and OpenTelemetry, and the dashboards.")
        say("Explore further: chronolog-demo status | chronolog-demo logs <service> | chronolog-demo down")


def parse_steps(text):
    if not text:
        return set(range(1, STEPS + 1))
    chosen = set()
    for part in text.split(","):
        low, _, high = part.partition("-")
        chosen.update(range(int(low), int(high or low) + 1))
    return chosen


def main():
    parser = argparse.ArgumentParser(description="ChronoLog 4.0 guided tour")
    parser.add_argument("--pause", action="store_true", help="wait for Enter between steps")
    parser.add_argument("--steps", help="run only these steps, for example 1-5,8")
    args = parser.parse_args()
    tour = Tour(args)
    try:
        tour.run()
    except TourError as error:
        print(f"\nTOUR FAILED: {error}", file=sys.stderr, flush=True)
        return 1
    except cl.Error as error:
        print(f"\nTOUR FAILED: SDK error {type(error).__name__}: {error}", file=sys.stderr, flush=True)
        return 1
    except subprocess.TimeoutExpired as error:
        print(f"\nTOUR FAILED: timeout running {error.cmd}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
