---
name: chronolog
description: Run, deploy, use and demo ChronoLog 4.0, a distributed, time-ordered, tiered log store that agents use as shared memory and provenance. Use when asked to start, stop or inspect a ChronoLog stack; write, replay or follow events; use the ChronoLog Python, C++ or TypeScript SDK; connect an agent through the chronolog-mcp server; use the kvs, pubsub, sql, stream, viz or ldms plugins; or explain and demonstrate ChronoLog to someone.
---

# ChronoLog 4.0

ChronoLog is a log store built for many writers on many machines. Every event gets a globally ordered timestamp (a hybrid logical clock, HLC) when a Keeper accepts it, is made durable in the Keeper's write-ahead log, flows to an archive tier, and can be replayed in order by anyone. Its defining property is honesty about completeness: every read ends with a Completion that says whether the answer is the whole truth for that time range, and if not, why. That makes it a safe shared memory for agents: an agent can tell "nothing happened" apart from "I cannot see everything yet".

Mental model in one line: chronicles hold stories, stories hold events, events are ordered by HLC, and reads come back with a verdict.

## Pick the path

| You want to | Go to |
|---|---|
| Bring up a stack and see it work in five minutes | Quickstart below |
| Use ChronoLog from your own code or as an agent | "Use it yourself" below, then references/api.md |
| Explain or present ChronoLog | "Demo it" below, then references/demo.md |
| Understand the guarantees and the moving parts | references/concepts.md |
| Deploy across machines, run dynamic membership, debug | references/operations.md |

## Quickstart

The demo kit lives in `deploy/demo/`. One script drives everything; every subcommand takes `--engine podman|docker` (default: podman when installed, else docker).

From a built source tree (needs the toolchain; see references/operations.md for building):

```bash
deploy/demo/chronolog-demo build          # native build, wheels, container images
deploy/demo/chronolog-demo up --full      # Visor, Keeper, Grapher, Player, plus InfluxDB, Grafana, viz, telemetry
deploy/demo/chronolog-demo tour --pause   # narrated, self-checking tour; Enter advances
deploy/demo/chronolog-demo down --volumes
```

From a portable bundle (no compiler; any Linux x86_64 host with Docker or Podman and Python 3.12):

```bash
cd chronolog-demo-bundle
./chronolog-demo load                     # load images, create .venv from the wheels
./chronolog-demo up --full
./chronolog-demo tour
```

A source tree makes a bundle with `deploy/demo/chronolog-demo bundle <dir>` (about 3 GB with Docker, 6 GB with Podman, since it carries the images). The target host needs libstdc++ and Python 3.12 or newer; nothing is compiled there.

`up` prints the endpoint table. Everything listens on 127.0.0.1 only:

| Service | Port | What it is |
|---|---|---|
| Visor (Catalog) | 50051 | chronicles, stories, writer acquisition; the address you connect to |
| Keeper | 50052 | accepts appends, assigns HLCs, WAL |
| Player (Replay) | 50054 | reads and tails |
| Grafana | 3000 | dashboards (with `--full`) |
| InfluxDB | 8086 | host telemetry exported from ChronoLog (with `--full`) |
| viz backend | 8087 | ChronoLog datasource for Grafana (with `--full`) |

`chronolog-demo status` shows health; `chronolog-demo logs <service>` shows one service's log (services: chrono-visor, chrono-keeper, chrono-grapher, chrono-player, and with `--full` influxdb, grafana, chrono-viz, chrono-stream-collect, chrono-stream-export).

Grafana (with `--full`): http://127.0.0.1:3000, login admin / admin. Dashboard `chronolog-stream` shows host telemetry that flowed through ChronoLog into InfluxDB; dashboard `chronolog-events` reads ChronoLog directly: type the chronicle the tour printed (tour-<unix time>) into its chronicle field.

## Use it yourself

Python is the quickest client. In a source tree the venv is `build/smoke-venv`; in a bundle it is `.venv`. Install elsewhere with `pip install chronolog-4.0.0-*.whl`.

```python
import chronolog as cl

client = cl.connect("127.0.0.1:50051", "127.0.0.1:50054", timeout=10)

def chronicle(name):
    try:
        return client.create_chronicle(name)
    except cl.AlreadyExists:
        return client.chronicle(name)

def story(chron, name):
    for s in client.list_stories(chron):
        if s.name == name and not s.tombstoned:
            return s
    return client.create_story(chron, name)

notes = story(chronicle("my-agent"), "notes")
with client.acquire(notes, "planner-1") as writer:          # one identity per logical writer
    r = writer.append(b'{"step": "plan", "text": "split the task"}',
                      content_type="application/json",
                      attributes={"gen_ai.agent.name": "planner"})
    print(r.event_id, r.hlc, r.durability, r.acked)          # acked is true only for DURABLE

end = cl.Hlc(r.hlc.physical_ns, r.hlc.logical + 1)            # ranges are half-open [start, end)
with client.read(notes, None, end) as reader:
    events = list(reader)
    print(reader.completion)                                 # complete, frontier, laggards, reason
```

Rules that keep an agent correct:
- Check `reader.completion.complete` before concluding anything from absence. A fresh write becomes readable as complete once the Keeper seals past it (sub-second with the demo settings); if `complete` is false, read again after a short wait or report the `reason` (`LAGGING_WRITERS`, `SOURCE_FAILED`, `TRUNCATED`, `PHYSICAL_AXIS_UNBOUNDED`).
- Appends default to DURABLE (fsynced in the Keeper WAL before the ack). `Durability.ACCEPTED` is faster and lives only in Keeper RAM until archived; a Keeper crash can lose it.
- Use `client.tail(story, after=event)` to follow a story live; never poll `read` in a loop. Tail never claims completeness.
- Retries are safe: the SDK retries with the same EventId and the Keeper deduplicates.
- Causality is automatic: whatever a client has read or tailed is ordered before its next append, across stories and machines. Share one Client per agent process so it carries what that agent has seen.
- Wall-clock questions ("what happened between 14:00 and 14:05") use the physical-time read; it is complete only when the writers' clocks were disciplined. See references/concepts.md.

Agents with MCP: register the bundled server with Claude Code against a running stack:

```bash
claude mcp add chronolog -- <venv>/bin/chronolog-mcp --catalog 127.0.0.1:50051 --player 127.0.0.1:50054 --chronicle agent-memory
```

Tools: `list_stories`, `create_story`, `append`, `read` (returns the Completion), `tail`, `start_chronolog`, `record_interaction`, `retrieve_interaction`, `stop_chronolog`; resource `chronolog://status`. Any MCP client takes the same command and arguments. OpenTelemetry users can export GenAI spans straight into ChronoLog with `chronolog[otel]` (references/api.md).

Command-line tools (in `build/dev/...` or the bundle's `bin/`): `chronolog_kvs` (versioned key-value), `chronolog_sql` (append-only tables with SELECT), `chronolog_admin` (cluster membership), `chronolog_stream_collect` and `chronolog_stream_export` (telemetry to InfluxDB). Usage strings are in references/api.md.

## Demo it

Run `chronolog-demo up --full` and `chronolog-demo tour --pause`, and talk over each step. The tour checks every claim it makes and stops on the first broken one, so a clean run is also proof the stack works. The talk track, short and long versions, and answers to the usual questions are in references/demo.md. The five points worth landing:

1. Many writers, one order: every event gets an HLC, and causality across agents is preserved.
2. Reads tell the truth: Completion says when an answer is whole, and why not when it is not.
3. Durable means durable: kill the Keeper mid-run and every DURABLE event comes back with the same id and timestamp.
4. Memory has tiers: hot in Keeper RAM and WAL, then an HDF5 archive, replayed as one ordered stream.
5. Built for agents: MCP server, OTel GenAI spans, and plugins (kvs, pubsub, sql, stream, viz, ldms) on the same SDK.

## When something is wrong

| Symptom | Likely cause | Fix |
|---|---|---|
| `Unavailable` on connect | stack not up, or wrong endpoint | `chronolog-demo status`; connect to 127.0.0.1:50051 |
| Read never becomes complete | the range end is above the sealed frontier, or a Keeper is down | end the range at or below the last HLC you need; check `completion.reason` and `laggards` |
| `OutOfRange` on append | a supplied physical timestamp is outside the Keeper's acceptance window (15 s back, 60 s ahead) | do not pass `physical=`; let the SDK stamp the event, keep your own times in attributes |
| `FailedPrecondition` on append | the writer was released or replaced by a newer incarnation | acquire again |
| Port already in use on `up` | another stack is running | `chronolog-demo down`, or stop the other stack |
| MCP server cannot import chronolog | wrong interpreter | run chronolog-mcp from the venv that has the chronolog wheel |

More in references/operations.md.
