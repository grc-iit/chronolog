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

Borrowed chunk/string APIs first reserve capacity, then take one immutable
snapshot. Copying or comparing arbitrary input has O(input-size) CPU/allocation
cost and cannot be independently preempted. That elapsed time consumes the
original deadline and prevents later dispatch after expiry. Serialization,
lookup, registration and native waiting run in charged work. This is a bound on
library waiting under normal OS scheduling, not a hard real-time promise for
arbitrary input sizes or operating-system stalls.

Each actual engine owner permits at most two source contexts and two incoming
bulk contexts. Incoming admission occurs before allocation, registration or
pull, across all senders. The receiver retains target bytes and native request
state until actual pull completion. It ingests or stages a fully decoded object
before replying. Both successful and failed response sends are contained; a
failed answer cannot discard an ingested chunk or escape the native callback.
At full incoming capacity the handler starts no rejection-response continuation.

A matching known reply proves that this receiver has finished all source access.
A local forward error, timer, resumed peer or elapsed interval does not. Failed
pulls publish no chunk receipt or query output. Zero failure bytes cannot match
a valid nonempty serialized object; empty raw sources are rejected locally.

Keeper attempt records compare every serialized chunk field, including event
payload, and distinguish destination protocol/address/provider and reporter.
Two partial chunks can share a story/time range and still be different attempts.
A late receipt stays available for its matching immutable content until the
originating chunk consumes it. There are at most two pending records per owner,
including known but unconsumed outcomes. Snapshot cleanup stays on the charged
worker. This can retain capacity indefinitely if no matching consumer returns.

The player uses six fixed control slots/workers: at most two query coordinators,
two response child slots reserved before hot work, and two global hot children.
Source and incoming bulk caps stay two. An admitted query can deliver its
incomplete result while two hot children remain unknown. The original scalar
ACK is still the exact query id; it acknowledges acceptance, not result delivery.
The client default runtime remains two.

There is no new payload-size limit. For up to two chunk attempts, memory includes
the original store-owned chunks, up to two immutable snapshots C1+C2 and up to
two encoded sources S1+S2. String growth can add temporary allocation capacity.
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
