# ChronoLog Paper Section III-B Design Delta

Updated: 2026-05-13 22:29:51 CDT

Source: `.agent/references/kougkas2020chronolog.pdf`, Section III-B, especially B.1 and B.2.

## What Section III-B Actually Says

Section III-B makes ChronoKeeper the gateway for tail operations and the highest available storage tier. It is not just an in-memory staging service. The Keeper is expected to manage NVRAM/NVMe-tier durable space for incoming events, acting as a high-tier cache over ChronoStore while holding the latest events.

B.1 defines the core Keeper structures:

- Distributed journal: incoming event data, distributed across Keeper servers, O(1) insertion, lock-free operations, and persistence on NVMe/SSD through HCL.
- Chronicle index: ordered eventIDs over the journal for replay, O(log n) lookup and insertion.
- Tail hashmap: lock-free tail publication, keyed by chronicle, storing the latest eventID per Keeper server so clients can find the tail without synchronization or locking.
- Event backlog: circular queue of events already copied from Keeper to ChronoStore, used to decide which journal records can be evicted.

B.2 defines the record path:

- Client calculates a ChronoTick.
- Client hashes the ChronoTick to select a Keeper server.
- Client sends one record RPC containing chronicle name, ChronoTick, and payload.
- Keeper inserts data into its local journal/data hashmap while updating the chronicle index and tail hashmap.
- The tail hashmap update is atomic and lock-free.
- The three Keeper operations are bundled in one RPC to avoid multiple round trips.
- The client can return after those Keeper-side operations complete.

## Delta Against The Current Optimization Direction

The paper-aligned append boundary is Keeper-local non-volatile admission, not archive/PFS completion and not merely memory acceptance. The local journal should behave as a WAL: once the event is persisted in the Keeper high-tier journal and its descriptor/tail metadata is published, the append can return under a durable Keeper semantics label. ChronoGrapher/archive movement is then a background drain from that journal.

The current codebase already has a Keeper-local journal path from previous iterations, but the design is still too influenced by the current implementation shape:

- The legacy timeline/archive path can still dominate completion or correctness gates unless explicitly separated.
- Several probes reduced lock scope or added owners/shards, but that is incremental around the current architecture.
- The paper's stronger design point is to avoid shared lock-heavy tail and journal metadata paths entirely: O(1) journal insertion, atomic tail publication, immutable descriptors/cursors, and background archive drain.
- Tail retrieval should be able to read recent data directly from the Keeper journal/data map, while historical replay uses lower tiers.

## Implication For Next Iterations

Do not spend the next step only shrinking an existing critical section unless profiling shows that exact lock dominates an accepted workload. The paper-backed direction is:

1. Make the benchmark semantics explicit: memory accepted, Keeper-WAL durable, archive/PFS durable, and read/tail source.
2. Use the Keeper-local journal as the durable append boundary, with fdatasync or the selected durable flush policy labeled.
3. Publish tail/journal descriptors atomically or through a single-owner/lock-minimized path.
4. Keep archive/timeline ingestion as an async consumer from persisted Keeper journal records.
5. Profile whether the remaining cost is client synchronous submission, RPC serialization, journal write/fsync, descriptor publication, tail reads, or downstream drain.

This is why Section III-B/B.1/B.2 should guide the next design iteration: the NVMe journal is primarily WAL/durable-return semantics, and lock-free tail metadata is the scalability mechanism around that durable journal.
