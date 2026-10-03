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
| Find an instance or start one for an agent | Local instances below, then references/local-instances.md |
| Run the compose demo | Compose quickstart below |
| Use ChronoLog from your own code or as an agent | "Use it yourself" below, then references/api.md |
| Explain or present ChronoLog | "Demo it" below, then references/demo.md |
| Understand the guarantees and the moving parts | references/concepts.md |
| Deploy across machines, run dynamic membership, debug | references/operations.md |

## Local instances

Use the installed `chronolog` launcher for agent memory. It needs Python 3.12; the `chronolog-local` wheel supplies the launcher, the `chronolog` wheel supplies RPC probes, and the `server` install component supplies the four executables. `chronolog doctor` names the executables it will run. In a source tree, `CHRONOLOG_BIN_DIR` can point to `build/dev`; installed servers can sit beside the launcher or on PATH.

Discover before starting anything:

```bash
chronolog doctor
chronolog ls
```

An explicit `CHRONOLOG_CATALOG`, with optional `CHRONOLOG_PLAYER`, overrides registry selection for `run` and `env`. Otherwise choose a `ready` instance by name; a compose demo started with `chronolog-demo up` appears as an external instance with engine and project metadata. For an existing ready instance, substitute its name for `default` in the commands below. A `degraded` instance needs diagnosis before use. If no suitable instance is ready, start the default:

```bash
chronolog up default
chronolog status default --probe
```

`up` waits for readiness. `status --probe` reports the RPC probe when the SDK wheel is installed, otherwise a TCP probe. Start the MCP process through the launcher so it holds a lease and receives the selected endpoints and shared lock directory:

```bash
chronolog run --up default -- chronolog-mcp --identity agent-memory/main --chronicle agent-memory
```

When MCP is already connected, use `instance_list(probe=true)` to discover and probe records, then `instance_control(action="attach", name="default")` for a ready instance or `instance_control(action="up", name="default", create=true)` to boot and bind it. Creation requires `create=true`; omit it when the record must already exist. A server with no configured endpoints or ready selection stays unbound and offers these tools instead of starting a stack implicitly. Close writable contexts before switching instances. The CLI path above supplies storage and port options that the MCP tools do not expose.

For Claude Code, use this command as the MCP server command with `claude mcp add chronolog -- chronolog run --up default -- chronolog-mcp --identity agent-memory/main --chronicle agent-memory`. Codex and clio-coder use the same command and arguments; see [local-instances.md](references/local-instances.md) for configuration, storage placement and recovery. Keep identities stable for one writer slot and give independently writing agents distinct identities.

The default policy keeps the instance running after the MCP process exits and drops its lease. Leave it up at session end. Stop it only when the user requests that, or when it is a throwaway ephemeral instance:

```bash
chronolog down default
```

`down` refuses a foreign live lease. Close its holder rather than forcing another agent off. Stopping preserves data and endpoints. For a throwaway run, `chronolog up scratch --ephemeral` sets the stop-on-last-detach policy; after its idle grace the supervisor stops it. Purge only disposable data with `chronolog down scratch --purge`.

In MCP, `instance_control(action="detach")` closes this server's contexts and drops its lease, preserving the default instance. `instance_control(action="down", name="default")` requests ordered stop only when the user asks; foreign leases still block it. A launcher wrapper holds a separate lease until its process exits. For MCP-created disposable instances, `on_last_detach="stop"` and `idle_grace_s` set the creation policy. Use the CLI for purge. See [api.md](references/api.md) for all twelve tools and the `since`/`until` acceptance-time bounds for recall and latest.

When boot or an RPC fails, inspect the executable paths and logs:

```bash
chronolog doctor
chronolog logs default --service supervisor --lines 40
chronolog logs default --service visor --lines 40
```

The marketplace uses the launcher when installed and otherwise keeps the `uvx chronolog-mcp==4.0.0` path for an already running deployment. Explicit harness configuration is the way to share a custom `CHRONOLOG_HOME`; the marketplace's existing environment allowlist is unchanged. The executable walkthrough is [scripts/local-walkthrough.py](scripts/local-walkthrough.py); run it from a built source tree with its smoke venv, launcher wheel and server component installed.

## Compose quickstart

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
- Check `reader.completion.complete` before concluding anything from absence. A fresh write becomes readable as complete once the Keeper seals past it; if `complete` is false, read again after a short wait or report the `reason` (`LAGGING_WRITERS`, `SOURCE_FAILED`, `TRUNCATED`, `PHYSICAL_AXIS_UNBOUNDED`).
- Appends default to DURABLE (fsynced in the Keeper WAL before the ack). `Durability.ACCEPTED` is faster and lives only in Keeper RAM until archived; a Keeper crash can lose it.
- Use `client.tail(story, after=event)` to follow a story live; never poll `read` in a loop. Tail never claims completeness.
- Retries are safe: the SDK retries with the same EventId and the Keeper deduplicates.
- Causality is automatic: whatever a client has read or tailed is ordered before its next append, across stories and machines. Share one Client per agent process so it carries what that agent has seen.
- Wall-clock questions ("what happened between 14:00 and 14:05") use the physical-time read; it is complete only when the writers' clocks were disciplined. See references/concepts.md.

For a local instance, register MCP through the launcher as above. To address a remote or manually configured stack directly, register the bundled server with Claude Code:

```bash
claude mcp add chronolog -- <venv>/bin/chronolog-mcp --catalog 127.0.0.1:50051 --player 127.0.0.1:50054 --chronicle agent-memory --identity agent-memory/main
```

Twelve tools: `instance_list`, `instance_control`, `context_open`, `context_remember`, `context_recall`, `context_latest`, `context_follow`, `context_reconcile`, `context_checkpoint`, `context_close`, `context_list`, `context_status`. A context is one story in the launcher's chronicle; `--identity` is the stable slot writable tools need, and each `agent` label is its own writer inside it. Reuse an operation_id when retrying after an error or timeout; pass every operation_id without a seen outcome to `context_reconcile`, since omitted ids may duplicate. Read `verdict` and `answer_complete` before concluding anything from absence. Any MCP client takes the same command and arguments. OpenTelemetry users can export GenAI spans straight into ChronoLog with `chronolog[otel]` (references/api.md).

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
