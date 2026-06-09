# ChronoLog May 31 Completion Tasks

## Goal

Deliver a credible, integrated ChronoLog cyberinfrastructure prototype by May 31, 2026: a small-scale but real implementation of the original ChronoLog architecture, adjusted to the current codebase and practical delivery constraints.

The target is not to claim production parity with Kafka or a fully general distributed time system. The target is to make ChronoLog visibly stronger, safer, and more complete for real application logging workloads:

- Visor-clock ChronoTick ordering.
- Keeper-resident hot journal, index, tail, and playback.
- Acceptance Time Window validation.
- Safer Keeper-to-Grapher archival.
- Multi-tier replay across archived HDF5 data and hot Keeper state.
- Clear runtime guarantees, bounded failure behavior, and end-to-end validation.
- Reproducible benchmarks and demos that show where ChronoLog's time-based design is valuable.

## Delivery Principles

- Build the algorithmic core at prototype scale rather than attempting every paper feature at production scale.
- Preserve the existing Keeper -> Grapher -> HDF5 -> Player data path while strengthening it.
- Add new typed APIs in parallel with compatibility wrappers where possible.
- Make every new feature testable through local deployment, integration tests, or deterministic unit tests.
- Fail clearly when a feature is unsupported rather than silently accepting ignored flags.
- Prefer explicit guarantees over broad claims.

## Public Completion Story

By project completion, ChronoLog should be presentable as:

> A distributed application logging prototype that uses a ChronoVisor-provided time base to produce ChronoTicks, validates append windows at the Keeper, maintains hot Keeper state for tail playback, archives ordered story data through ChronoGrapher to HDF5, and replays data across hot and archived tiers. The system is validated with real logging workloads, bounded failure handling, and benchmark comparisons against Kafka under scoped conditions where ChronoLog's time-indexed design is expected to help.

## Core Architecture Tasks

### 1. Visor Clock And ChronoTick Model

- Add first-class types for `ChronoTick`, `ChronicleId`, `StoryId`, `EventId`, clock base, clock offset, drift/rate, latency, and uncertainty.
- Add a Visor clock service based on `std::chrono` as the cluster time authority.
- Extend client connect response to include Visor clock state.
- Add a public `SyncClock` client API.
- Refresh clock state on connect, explicit sync, chronicle acquisition, and relevant reconnect paths.
- Store chronicle base time at creation.
- Compute client ChronoTicks from Visor sync state rather than raw local timestamps.
- Keep a compatibility path for current story logging while internally routing through the new time model.
- Add deterministic clock tests with fake client skew, drift, and latency.
- Add integration tests showing multiple clients produce comparable ChronoTicks under one Visor.

### 2. Keeper Hot Journal, Index, Tail, And Backlog

- Add a Keeper hot-state module with explicit interfaces:
  - append event
  - fetch event by ChronoTick/EventId
  - fetch tail by chronicle/story
  - scan range
  - expose archive/backlog watermark
  - evict acknowledged archived ranges
- Implement an in-memory backend first.
- Add clear extension points for persistent or distributed backends later.
- Update record ingestion so success means the event reached Keeper hot state, not just an intermediate queue.
- Maintain an ordered per-story/per-chronicle index by ChronoTick.
- Maintain per-story/per-chronicle tail state.
- Track backlog entries that have not yet been archived.
- Track last archived/replayable watermark.
- Add capacity limits and return explicit backpressure/capacity errors.
- Add unit tests for append, duplicate detection, range scan, tail lookup, backlog watermark, and eviction.

### 3. Acceptance Time Window And Collision Semantics

- Add per-acquisition ATW metadata derived from configured defaults and measured/client-observed latency.
- Reject records whose ChronoTick falls outside the accepted window.
- Return a retryable backdated/outside-window error.
- Add a simple retry helper on the client side where appropriate.
- Add collision policy metadata to chronicle/story creation.
- Implement a minimum collision policy set:
  - ordered-list-by-arrival for same tick
  - first-wins or duplicate-reject mode
- Reject unsupported collision policies explicitly.
- Preserve deterministic ordering within collided ticks.
- Add tests for outside-ATW rejection, accepted near-late events, duplicate events, and same-tick collision handling.

### 4. Keeper Tail Playback

- Add a public `Playback` API for tail reads.
- Add Keeper RPCs for tail lookup and event fetch, or one combined tail playback RPC.
- Ensure tail playback reads Keeper hot state and does not depend on HDF5 archival or ChronoPlayer.
- Return tail metadata with the event: ChronoTick, client id, index, story/chronicle id, and source tier.
- Add C++ and Python bindings for playback.
- Add CLI support for tail playback.
- Add integration tests with multiple clients and multiple Keepers.
- Add a benchmark measuring tail playback throughput and latency separately from replay.

## Grapher And Archive Tasks

### 5. Safer Keeper-to-Grapher Transfer

- Make extraction chain return aggregate success/failure.
- Prevent chunk deletion until required sinks acknowledge success.
- Add retry/outage buffers for failed RDMA/HDF5 transfers.
- Add configurable retry count and retry backoff.
- Add queue depth and failed-transfer metrics.
- Add explicit error classes for transfer failure, timeout, receiver unavailable, serialization failure, and archive failure.
- Add tests for transfer failure and retry behavior.

### 6. Atomic HDF5 Archive Publication

- Validate archive directory existence, directory type, and writeability at startup.
- Fail startup on invalid archive paths; do not fall back to `/tmp`.
- Write HDF5 chunks to temporary file names.
- Close and flush before publishing.
- Publish completed archives with atomic rename.
- Make Player ignore temporary/incomplete files.
- Add optional fsync policy for file and parent directory when durability mode requires it.
- Add crash-injection or simulated partial-write tests.
- Add archive metadata sufficient to identify chronicle, story, time range, chunk sequence, and publication state.

### 7. Archive Layout And Configuration

- Generate separate output roots:
  - Keeper CSV/debug output
  - Grapher HDF5 archive
  - Player archive scan/index state
  - logs/monitoring
- Make Player scan only the HDF5 archive root.
- Update local deploy config generation.
- Update clean behavior to remove only managed ChronoLog subdirectories.
- Document local and installed directory layout.
- Add smoke tests that validate generated configs and directory separation.

### 8. Grapher Watermarks

- Track last received, last written, and last published ChronoTick per chronicle/story.
- Report archival lag from Keeper hot state to HDF5 archive.
- Expose watermarks through logs, status command, and/or RPC.
- Use watermarks to decide when Keeper backlog can be evicted.
- Add tests for archive watermark advancement and recovery after Grapher restart.

## Player And Replay Tasks

### 9. Replay Query Contract

- Remove hard-coded replay query id values.
- Carry query id end to end through request and response.
- Include query id in serialized response payloads.
- Replace one-second polling with condition-variable or Argobots eventual completion.
- Add configurable replay timeout/deadline.
- Add replay cancellation or cleanup on timeout.
- Return distinct statuses for:
  - no player available
  - no archived files
  - not yet replayable
  - no matching events
  - query timeout
  - transport failure
  - partial response
- Add concurrent replay tests with overlapping and independent ranges.

### 10. Replay Planner

- Add a replay planner/resolver component.
- Accept replay requests with:
  - chronicle/story id
  - one range or vector of ranges
  - optional no-end range
  - optional constraint/filter
  - deadline
  - consistency/source-tier options
- Split requested ranges into archive ranges and hot Keeper ranges.
- Use archive watermarks to determine which tier owns each range.
- Query HDF5 archive for persisted ranges.
- Query Keeper hot state for latest unarchived ranges.
- Merge and sort replay results by ChronoTick and tie-breakers.
- Return source-tier metadata for observability.
- Add tests covering archive-only, Keeper-only, and mixed-tier replay.

### 11. Replay Constraints And Deduplication

- Add a minimal constraint representation for time/range and simple event metadata filters.
- Reject unsupported constraints explicitly.
- Deduplicate overlapping replay ranges.
- Deduplicate duplicate events across hot/archive overlap using event identity.
- Add replay epoch batching if feasible for near-simultaneous overlapping requests.
- Add tests for overlapping ranges, constraints, and hot/archive boundary duplicates.

### 12. Player Archive Index

- Add an explicit archive manifest or index keyed by chronicle/story/time range.
- Avoid broad recursive scans on every replay.
- Update the index from file creation/inotify/polling events.
- Persist enough index state for Player restart.
- Add scan duration and file count metrics.
- Add tests with large synthetic archive trees.

## Client API And Binding Tasks

### 13. Typed Client API

- Add typed APIs in C++:
  - `SyncClock`
  - `CreateChronicle`
  - `AcquireChronicle` or typed acquisition wrapper
  - `Record`
  - `Playback`
  - `Replay`
  - `Release`
- Keep legacy story APIs as compatibility wrappers.
- Add structured options objects for record, playback, replay, and timeouts.
- Add structured result objects with status, event ids, source tier, and diagnostics.
- Document current guarantees for each API.
- Add examples for writer, tail reader, replay reader, and mixed hot/archive replay.

### 14. Python Binding Hardening

- Wrap C++ handles in Python-safe objects.
- Prevent use-after-release and use-after-client-destroy.
- Release the GIL around blocking connect, record, playback, replay, release, and disconnect calls.
- Expose timeouts and structured errors in Python.
- Add Python playback and replay examples.
- Add Python import smoke tests.
- Add Python use-after-release tests.
- Add package metadata or a packaging target that records ABI and ChronoLog version.

### 15. CLI And Developer UX

- Add CLI commands for:
  - connect/smoke status
  - create chronicle/story
  - record event
  - playback tail
  - replay range
  - show watermarks
  - show service health
- Make CLI errors map to the new status classes.
- Add a simple demo script that writes and replays application logs.
- Add a local "doctor" or "status" command for deployment readiness.

## Reliability And Operations Tasks

### 16. RPC Deadlines And Error Codes

- Add configurable RPC timeout/deadline options.
- Use timed Thallium calls for synchronous RPCs.
- Map timeout separately from generic failures.
- Add retry policy where safe:
  - connect
  - acquire
  - record
  - release
  - playback
  - replay request
  - RDMA/bulk transfer
- Add status codes for timeout, unavailable, backpressure, not replayable, partial success, unsupported option, invalid clock, and archive failure.
- Add tests using stopped or blackholed services.

### 17. Bounded Queues And Backpressure

- Replace unbounded hot-path queues with bounded queues where feasible.
- Add queue capacity configuration.
- Add blocking, timeout, or reject behavior based on per-queue policy.
- Add queue depth metrics.
- Add high-water logging.
- Add tests showing producers receive backpressure rather than causing unbounded memory growth.

### 18. Release, Disconnect, And Reconnect Safety

- Do not delete local story handles before server release succeeds.
- Make release retryable.
- Make disconnect retryable.
- Add explicit forced-close behavior for destructors and shutdown paths.
- Make duplicate connect preserve existing client state or return a clear duplicate-client error.
- Add reconnect/resume or reconnect/force-clean semantics.
- Add tests for release failure, disconnect failure, duplicate connect, and client crash cleanup.

### 19. Local Deployment Supervision

- Add PID files or process-group tracking for local deployment.
- Add readiness probes before starting dependent services.
- Add a status command that reports:
  - role
  - PID
  - endpoint
  - readiness
  - log file
  - archive/watermark status if available
- Stop only managed PIDs.
- Escalate from graceful stop to force kill only after timeout.
- Fail startup if any component exits early.
- Add deployment smoke tests.

### 20. Observability

- Add structured logs for:
  - client connect/sync
  - record accepted/rejected
  - ATW failures
  - collision handling
  - Keeper tail update
  - Grapher archive publish
  - replay query lifecycle
  - hot/archive replay split
  - queue backpressure
  - RPC timeout/retry
- Add counters or status output for:
  - events accepted
  - events rejected by reason
  - events archived
  - replay requests
  - replay timeouts
  - tail playback requests
  - queue depths
  - archive lag
- Add concise operator-facing status summaries.

## Testing And Validation Tasks

### 21. Unit Tests

- Clock sync state and ChronoTick computation.
- Keeper hot journal append/fetch/range/tail.
- ATW acceptance and rejection.
- Collision policy behavior.
- Backlog and archive watermark behavior.
- Replay planner range splitting.
- Replay result merge/sort/dedupe.
- HDF5 atomic publication naming and filtering.
- Bounded queue behavior.
- Error-code mapping.

### 22. Integration Tests

- Multi-client record with Visor-clock ChronoTicks.
- Keeper tail playback without ChronoPlayer.
- Keeper -> Grapher -> HDF5 archive publication.
- Player archive replay.
- Mixed Keeper hot plus HDF5 archive replay.
- Replay timeout and no-data behavior.
- Release/disconnect retry safety.
- Local deployment readiness and shutdown.

### 23. End-To-End Application Logging Tests

- Build a representative application logging workload:
  - structured JSON-like logs
  - multiple producers
  - multiple stories/streams
  - tail reader
  - range replay reader
  - mixed hot/archive replay
- Verify:
  - event count
  - event ordering
  - story/chronicle identity
  - no duplicate events across replay boundaries
  - archive watermark progress
  - bounded replay timeout
  - clean shutdown
- Produce logs and summary artifacts suitable for NSF reporting.

### 24. Benchmark Tasks

- Add benchmark modes for:
  - append throughput
  - tail playback throughput/latency
  - archive replay throughput
  - mixed hot/archive replay latency
  - ingestion-to-archive lag
  - ingestion-to-replayable lag
- Compare against Kafka under scoped workloads:
  - local single-node logging
  - multi-producer append
  - tail-read-like latest event access
  - time-range replay over persisted data
- Report where ChronoLog wins, ties, or loses.
- Avoid claiming general Kafka replacement.
- Include benchmark configuration, hardware, payload sizes, producer counts, retention/archive layout, and failure conditions.

### 25. Customer/Use-Case Demos

- Add at least one real-use-case style demo:
  - HPC application telemetry
  - distributed service logs
  - simulation checkpoint/event trace
  - IoT/sensor time-series ingestion
- Include writer, tail monitor, replay analytics, and archive inspection.
- Provide a reproducible script from build/install to demo output.
- Capture output suitable for slides and final reporting.

## Documentation Tasks

### 26. Current Guarantees Document

- Document what ChronoLog guarantees now:
  - record acceptance
  - ChronoTick basis
  - ATW behavior
  - tail playback source
  - archive durability boundary
  - replayable watermark
  - failure and timeout behavior
- Document what is prototype-scale.
- Document what remains future work.
- Avoid overclaiming production-scale clock, replication, or Kafka-equivalent semantics.

### 27. Architecture Document

- Update architecture docs to describe the implemented small-scale architecture:
  - ChronoVisor clock
  - client ChronoTick computation
  - Keeper hot state
  - Grapher HDF5 archive
  - Player replay planner
  - hot/archive replay
  - observability and watermarks
- Include diagrams or clear flow descriptions.
- Explain how this maps to the original ChronoLog paper design and where it is intentionally scoped down.

### 28. Developer And Operator Docs

- Update build/install instructions.
- Update local deployment instructions.
- Add troubleshooting for common failures:
  - service not ready
  - archive path invalid
  - replay not yet available
  - timeout
  - backpressure
  - stale client acquisition
- Add examples for C++, Python, and CLI.

### 29. NSF Completion Package

- Prepare a final technical summary:
  - implemented architecture
  - validation results
  - benchmark results
  - supported use cases
  - integration readiness
  - remaining research/production work
- Prepare demo scripts and artifacts.
- Prepare benchmark plots/tables.
- Prepare a concise limitations section that is honest but confident.
- Prepare a roadmap beyond May 31 for production distributed scaling.

## Cut Lines

The following are valuable but should not block the May 31 package unless core work is already stable:

- Fully distributed persistent HCL-backed Keeper journal.
- Production-grade replication.
- Chronicle migration across clusters.
- All four collision policies.
- Elastic autoscaling of Grapher/Player resources.
- External object store/Hadoop/DataLake connectors.
- Full Kafka-compatible consumer group semantics.
- Strong global-time correctness beyond the Visor-clock prototype model.
- Complete public API migration away from legacy story methods.

## Success Criteria

- A developer can start ChronoLog locally, write real application log events, tail-read the latest event, archive data, replay by time range, and stop cleanly.
- Record, playback, and replay have bounded failure behavior and useful error codes.
- Accepted events are not silently discarded on common transfer/archive failures.
- HDF5 archive files are published safely and read only after completion.
- Replay can combine archived data with hot Keeper state for near-tail ranges.
- Tests cover the new clock, Keeper, ATW, playback, archival, replay, and deployment behavior.
- Benchmarks demonstrate scoped advantages and provide honest comparison against Kafka.
- Documentation clearly explains current guarantees, limitations, and integration path.
