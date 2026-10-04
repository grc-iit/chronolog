# ChronoLog concepts

ARCHITECTURE.md at the repository root is the authority; this is the working summary. Invariant ids (I6.5, I8.9 ...) point into it.

## Data model

- Chronicle: a namespace of stories.
- Story: one append-only log, identified by a numeric `story_id` and a name inside its chronicle.
- Event: an envelope (payload bytes, content_type, attributes, optional trace_id and span_id) plus three time-related fields:
  - `id` (EventId): (story_id, writer_id, incarnation, sequence). The identity used for idempotent retries and deduplication.
  - `hlc`: {physical_ns, logical}, assigned by the Keeper when it accepts the event. Total order within a story; causally consistent across stories and machines.
  - `physical`: the writer's wall-clock reading with its uncertainty and clock status, used for wall-clock queries.
- Writer: a client identity acquired on a story. Each acquisition gets a new incarnation, and appends carry consecutive sequence numbers, so a dead writer's late appends can be fenced off.

## The four services

| Service | Role |
|---|---|
| Visor | Hosts the Catalog (chronicles, stories, writer acquisition, routes) and cluster membership. Can run as a three-replica Raft group. |
| Keeper | Accepts appends, assigns HLCs, keeps recent events in RAM and a write-ahead log, seals frontiers, ships chunks to the archive. |
| Grapher | Receives sealed chunks from Keepers and publishes them to the archive (HDF5 chunk files plus a manifest log), on local disk or shared NFS/PFS. |
| Player | Serves Replay: merges hot data from Keepers with archived data into one ordered, deduplicated stream, and decides completeness. |

Data path: client asks the Visor to acquire a writer and gets its route; appends go to the assigned Keeper (RAM, then WAL fsync for DURABLE); the Keeper ships sealed chunks to the Grapher, which publishes them to the archive and confirms with a receipt; the Keeper evicts only what the archive confirmed; reads go to the Player, which reads Keepers above their eviction floor and the archive below it.

```
writer ──append──▶ Keeper (RAM + WAL) ──chunks──▶ Grapher ──▶ archive (HDF5)
   │                   ▲ hot reads                              ▲ cold reads
   └─acquire─▶ Visor   └──────────── Player (Replay) ───────────┘ ◀──read/tail── reader
```

## Durability

- ACCEPTED: in Keeper RAM and ordered. Lost if the Keeper crashes before the chunk is archived.
- DURABLE (default): fsynced in the Keeper WAL with group commit before the ack. Survives a Keeper crash; the restarted Keeper replays its WAL with the same ids and HLCs.
- Only DURABLE is an ack in the SDK sense (`AppendResult.acked`). UNSPECIFIED in a request means DURABLE.

## Completeness

Each Keeper keeps a sealed frontier F: every event with hlc < F is visible, and no event with hlc < F will ever be ingested later. A Read over [start, end) is complete when every Keeper in the story's route answered and its sealed frontier reached `end`. The Read's last message is a Completion: `complete`, `frontier`, `laggards` (writers holding it back) and `reason`:

| reason | meaning |
|---|---|
| LAGGING_WRITERS | some Keeper has not sealed up to `end` yet; inspect the reported frontier and laggards |
| SOURCE_FAILED | a Keeper or the archive did not answer, or a range was abandoned after a failure |
| TRUNCATED | the read hit a size limit; the returned prefix is still complete up to the reported frontier |
| PHYSICAL_AXIS_UNBOUNDED | a wall-clock read selected an event whose writer clock had no bound |

Tail follows a story live and never claims completeness (I6.5). A complete read stays complete for DURABLE events: repeating it returns the same DURABLE events. ACCEPTED events that a Keeper crash destroyed before they were archived disappear from later reads, which is what ACCEPTED trades for speed.

## Time

- HLC: assigned by the Keeper, always increases, and orders events even when machine clocks drift. Causality is automatic: the SDK remembers the highest HLC a client has read, tailed or written and sends it as the causal floor of that client's next append, so anything an agent has seen is ordered before what it writes next, even across stories and Keepers (I8.5).
- Physical time: the writer's wall-clock reading. Each Keeper runs an acceptance policy (fixed when the Catalog is created): it rejects a reading more than 15 s in the past or 60 s in the future of its acceptance clock with OUT_OF_RANGE, and the rejection consumes that sequence number (I8.9). A reading counts as bounded when the clock is Synced (NTP-disciplined) with uncertainty under 1 s. A wall-clock read is complete only for a story whose Keepers all ran the policy, when every Keeper's physical frontier passed the end, and when every selected event was bounded (I6.10).

## Membership modes

- Static (default): a fixed set of Keepers from configuration; every story is at epoch 1.
- Dynamic: the Catalog is replicated across three Visors with Raft; Keepers can be drained, joined or abandoned while running (`chronolog_admin`). Ordering survives handoffs through committed HLC ceilings and per-story cuts, readers keep reading retired Keepers until their data is archived, and lost data is reported as SOURCE_FAILED rather than silently missing.

## Plugins

All are built on the public SDKs and inherit the guarantees above.

| Plugin | What it gives you |
|---|---|
| kvs (`plugins/kvs`) | versioned key-value store: put, get, get as of an HLC, history |
| pubsub (`plugins/pubsub`) | topics with at-least-once delivery, resumable consumers, positions saved in kvs |
| sql (`plugins/sql`) | append-only typed tables with a small SELECT that reports Completion |
| mcp (`plugins/mcp`) | MCP server exposing contexts to agents: remember, recall, latest, follow and reconcile with explicit completeness |
| stream (`plugins/stream`) | host metrics into ChronoLog and a resumable exporter to InfluxDB with Grafana dashboards |
| viz (`plugins/viz`) | Grafana datasource for ChronoLog with completeness notices and live tail |
| ldms (`plugins/ldms`) | C bridge for LDMS sampler data with a non-blocking queue |
