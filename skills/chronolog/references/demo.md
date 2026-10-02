# Presenting ChronoLog

Setup: `chronolog-demo up --full`, then open Grafana at http://127.0.0.1:3000 (admin / admin): `chronolog-stream` for host telemetry, `chronolog-events` for the tour's events (type the chronicle name the tour prints). From another machine, forward the ports first: `ssh -N -L 3000:127.0.0.1:3000 -L 50051:127.0.0.1:50051 -L 50052:127.0.0.1:50052 -L 50054:127.0.0.1:50054 <host>`. Run `chronolog-demo tour --pause` and press Enter to advance. Every step checks its own claim, so a broken stack shows up as a failed step, never as a wrong story.

## Two-minute version

"Agents need a memory they can share and trust. ChronoLog is a log store where many writers on many machines append events, every event gets one global timestamp, and every read tells you whether it is the complete answer. Watch: two agents write, a third reads; we kill the storage process mid-run and every durable event comes back; and the read always says when it is whole and when it is not."

## Fifteen-minute version, step by step

| Tour step | Say | Point at |
|---|---|---|
| 1 Catalog | Chronicles group stories; a story is one append-only log, like a channel or a trace. | the three stories created |
| 2 Write | Each append gets an EventId (who and which sequence) and an HLC (when, in one global order). DURABLE means fsynced in the Keeper's write-ahead log before the ack; ACCEPTED is faster and in RAM only. | the ids, HLCs, achieved durability |
| 3 Completeness | The read ends with a verdict. complete=true means nothing in that range is missing or still to arrive. Asking past the frontier gives complete=false and the reason. Most stores cannot tell you this. | complete, frontier, reason |
| 4 Shared memory | Agent B follows a story live while agent A writes; B sees A's events in order, as they land. | the tail output |
| 5 Causality | B read A's note before writing its own, so B's event is ordered after A's, even though they write through different processes. The SDK carries this automatically. | the two HLCs |
| 6 Crash | We SIGKILL the Keeper. During the outage the read says SOURCE_FAILED instead of quietly returning less. After restart every DURABLE event is back with the same id and HLC. | the outage Completion, the recovered ids |
| 7 Archive | Sealed data moves to the Grapher's HDF5 archive; reads stitch hot and archived data into one ordered stream. | archive receipts or the cold read |
| 8 Wall-clock | "What happened between 14:00 and 14:05?" is a different question from HLC order. ChronoLog answers it completely only when the writers' clocks were disciplined, and says so when they were not. | clock status, Completion |
| 9 Plugins | The same guarantees power a versioned key-value store, pub/sub with resumable consumers, and SQL over append-only tables. | kvs history, pubsub delivery, SQL Completion |
| 10 Agents | The MCP server gives any agent these tools; OpenTelemetry GenAI spans land in stories per conversation. | MCP tool list, the `claude mcp add` line |
| 11 Dashboards | Host telemetry flows through ChronoLog into InfluxDB, and Grafana reads ChronoLog directly, with a warning on any panel whose data is incomplete. | Grafana dashboards |

## Questions people ask

- How is this different from Kafka? Kafka orders within a partition and cannot tell a reader that a time range is complete across producers. ChronoLog gives one causal order across all writers and a per-read completeness verdict, and it archives into HDF5 for HPC-scale history.
- What happens when a machine dies? DURABLE events survive in the WAL. With dynamic membership (three Visor replicas), a dead Keeper is replaced; readers keep reading its data until it is archived, and anything truly lost is reported as SOURCE_FAILED for exactly that range. On a three-node homelab with an NFS archive, killing the Visor leader recovered in 2.4 s and a Keeper partition in 7.8 s, with no acknowledged DURABLE event lost or reordered.
- Why HLC and not wall-clock time? Wall clocks disagree across machines. The HLC never goes backward and respects causality; wall-clock queries are still supported, with honest completeness.
- What does it cost to write? An ACCEPTED append is an in-memory insert; DURABLE adds a grouped fsync. Batching amortizes both.
- Where does it run? Containers on a laptop or server for development, and natively across nodes with shared NFS or a parallel file system for the archive on clusters.
