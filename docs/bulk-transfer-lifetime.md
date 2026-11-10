# Bulk transfer lifetime

Chunk and query-response transfers use one owned context per attempt. The
context keeps the actual engine/provider owner, immutable input, serialized
bytes, endpoints, callable and bulk registration alive. A caller timeout does
not cancel a remote pull or prove that the source can be freed.

The transfer ceiling is 30000 ms. This implementation issues one descriptor
forward and waits independently for at most min(5000 ms, remaining transfer
budget). Preparation, lookup and native work share that attempt deadline. The
native receipt forward can continue after the waiter returns. Unknown native
work keeps its slot and registration; it is never replaced or retried merely
because the caller timed out.

A keeper partial chunk is serialized once, on the extraction thread, together
with its CRC-32C. Every transfer of it still in progress, to the player and to
the grapher, and a late receipt waiting to be collected share those bytes; the
chunk holds them weakly, so they are freed once no transfer uses them, and any
change to the chunk forgets them. Borrowed string APIs and query responses
still take one immutable copy. Copying or comparing arbitrary input has
O(input-size) CPU/allocation cost and cannot be independently preempted. That elapsed time consumes the
original deadline and prevents later dispatch after expiry. Serialization,
lookup, registration and native waiting run in charged work. This is a bound on
library waiting under normal OS scheduling, not a hard real-time promise for
arbitrary input sizes or operating-system stalls.

Each actual engine owner permits at most two source contexts and two incoming
bulk contexts. See *Receive slots* below for the incoming side. The receiver
retains target bytes and native request state until the pull has returned. It
ingests or stages a fully decoded object before replying. Both successful and
failed response sends are contained; a failed answer cannot discard an ingested
chunk or escape the native callback.

## Receive slots

The grapher's recording MID and the player's recording MID each hold two
receive slots (BulkReceives), shared by every keeper that sends them keeper
partial chunks. A receive_story_chunk takes a slot before it allocates the
target buffer, registers it or pulls. The slot holds the request, the target
buffer and its registration until the handler is done with them.

- No free slot: the receiver answers CL_ERR_RPC_CAPACITY in ChunkReceipt.status,
  having pulled nothing, and the keeper tries the chunk again after 1 s,
  doubling to 30 s. At most two such refusals are sent at a time; beyond that
  the request is not answered. Without this answer the keeper's untimed chunk
  forward waited forever.
- The pull moves the chunk in pieces of transfer_piece_bytes (1 MiB by
  default), one after the other, then checks the CRC-32C the keeper sent. A
  mismatch is refused like a chunk that fails to decode.
- A pull or a reply that returned with an error frees its slot: Mercury has
  completed the operation, so nothing reads the target any more. Only an
  exception between starting a piece and waiting for it keeps the slot, since
  that piece may still be in flight.
- A pull that never returns keeps its slot. Margo 0.13.1 cannot time or cancel
  a bulk transfer; margo_bulk_itransfer_timed exists from Margo 0.19.2, and
  thallium 0.17 wraps it as remote_bulk::timed().
- The grapher's stop waits for both slots to be free.

A matching known reply proves that this receiver has finished all source access.
A local forward error, timer, resumed peer or elapsed interval does not. Failed
pulls publish no chunk receipt or query output. Zero failure bytes cannot match
a valid nonempty serialized object; empty raw sources are rejected locally.

Keeper attempt records compare the serialized bytes, and distinguish
destination protocol/address/provider and reporter. Two partial chunks can
share a story/time range and still be different attempts. A late receipt stays
available for its matching bytes until the originating chunk consumes it. There
are at most two pending records per owner, including known but unconsumed
outcomes. Dropping the bytes stays on the charged worker. This can retain
capacity indefinitely if no matching consumer returns.

The grapher remembers each delivery it received: the reporting keeper, range,
size, CRC-32C and a 64-bit hash. A resend of a delivery whose events are already
written gets the first receipt and is not ingested again; a resend of one still
unwritten is ingested again, since that is how events whose write failed are
written. A delivery is forgotten duplicate_receipt_hold_secs (600 by default)
after its receipt is written. A restarted grapher remembers nothing.

The player uses six fixed control slots/workers: at most two query coordinators,
two response child slots reserved before hot work, and two global hot children.
Source and incoming bulk caps stay two. An admitted query can deliver its
incomplete result while two hot children remain unknown. The original scalar
ACK is still the exact query id; it acknowledges acceptance, not result delivery.
The client default runtime remains two.

There is no new payload-size limit. For a chunk in flight, memory includes the
original store-owned chunk and one serialized copy S, shared by its player and
grapher transfers and their retries. String growth can add temporary allocation capacity.
Each receiver adds at most two target buffers T1+T2 and decoded objects. Direct
stream buffers avoid a second full serialized stream copy. Query responses
share their already-owned prepared events with serialization. Count bounds do
not imply constant RSS independent of legal input size.

Reader bulk callbacks hold the existing ClientRpcContext native lease and stage
events in owned query state. They keep no caller vector reference.
The public replay caller publishes staged vector data. The owned callback runs
on the receive thread outside map locks. An atomic close/active gate stops new
callbacks at expiry/return and waits only for an already running cooperative
callback to finish. Outbound controls use the original replay entry deadline;
see [Replay deadlines and retained queries](replay-deadlines.md).

Native provider/main-stack retirement remains incomplete until all incoming,
bulk and native access is proved settled. The scope gates keep the process and
referenced stores alive on incomplete stop. They do not force exit or claim
that a copied raw-MID wrapper prevents explicit native finalization.

Build through tools/deploy, then run:

```bash
BUILD_DIR=$HOME/chronolog-build/bounded-rpc-monotonic/Debug \
  tests/integration/rpc_bulk_timeout_test.sh
```

That script runs communication fault fixtures on both native transports. It
does not deploy daemons. Run the standard-config replay_fanout_timing_test.sh
separately for the real daemon regression. Loopback TCP interface overrides
apply only to explicitly all-loopback tests, never distributed deployments.
Harness process termination is cleanup, not a production settlement proof.

Watermark reports use the grapher's data-admin MID and its fixed two-slot
runtime. Each pass uses one attempt budget (5000 ms by default), including
preparation, provider registration, lookup and forwarding. Capacity is reserved
before copying report maps. Targets beyond available slots rotate through
later passes. Definite failures keep quiet progress pending with bounded
backoff. An unknown attempt is not retried and keeps its target from being
parked, since a cancelled report may still reach the keeper, but its slot comes
back once its forward has returned. Over ofi+tcp a forward to a closed port can
take about 90 s to return after its timeout. No new engine or runtime bypasses
that limit.

A successful one-way report proves local send completion only. It does not
confirm Keeper application or create an archive receipt. Each admitted attempt
reads the current complete watermark/receipt envelope; report failure does not
advance persistence or settle a receipt. Local incarnation tokens invalidate
queued reports after drop/recreate. They cannot revoke an already dispatched
old drop: the existing wire has no story incarnation, so that documented race
remains. Final forced publication still obeys admission, deadlines, generations
and native ownership. Complete incoming/provider/MID retirement remains B12.
