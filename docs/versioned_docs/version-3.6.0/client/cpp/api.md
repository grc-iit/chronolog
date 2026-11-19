---
sidebar_position: 2
title: "API Reference"
---

# C++ Client API

All types described here are defined in `chronolog_client.h` under the `chronolog` namespace.

```cpp
#include <chronolog_client.h>
```

## Wire-protocol version

```cpp
static constexpr uint32_t chronolog::CLIENT_PROTOCOL_VERSION;
```

The wire-protocol version exchanged on `Connect()`. The Visor returns `CL_ERR_PROTOCOL_VERSION_MISMATCH` if a connecting client's version doesn't match the server's expected version. The constant is bumped whenever the wire format between the client and any ChronoLog server component changes in an incompatible way.

:::warning
A client built against ChronoLog 3.4.0 or older cannot reach 3.5.0 servers at all, and the reverse holds too. 3.5.0 moved from Mercury 2.2.0 and Margo 0.13.1 to Mercury 2.4.1 and Margo 0.24.4, which receive RPC requests in a different form, so an older client's `Connect()` waits without an answer instead of returning `CL_ERR_PROTOCOL_VERSION_MISMATCH`. Rebuild clients against 3.5.0, and run the visor, keepers, graphers and players at the same version.

3.6.0 keeps `CLIENT_PROTOCOL_VERSION` at 3, but its components and clients are not tested against 3.5.0 ones: a keeper's chunk send to the grapher, for one, carries a checksum that 3.5.0 does not send. Mixed versions are not supported; run every component and client at 3.6.0.
:::

---

## `Client` Class

The `Client` class manages the connection to ChronoVisor and provides all chronicle, story, and event operations.

### Constructors

```cpp
// WRITER mode — event logging only
Client::Client(ClientPortalServiceConf const& portalConf);

// WRITER + READER mode — event logging and story replay
Client::Client(ClientPortalServiceConf const& portalConf,
               ClientQueryServiceConf const& queryConf);
```

- The first constructor creates a write-only client connected to the ChronoVisor portal service.
- The second constructor additionally connects to the ChronoPlayer query service, enabling `ReplayStory`. Calling `ReplayStory` on a write-only client returns `CL_ERR_NOT_READER_MODE`.

### Connection

```cpp
int Client::Connect();
```

- Establish a session with ChronoVisor. The client advertises `CLIENT_PROTOCOL_VERSION` as part of the handshake.
- Returns `CL_SUCCESS` on success, `CL_ERR_PROTOCOL_VERSION_MISMATCH` if the server rejects the client's protocol version, or another error code on failure.
- The first `Connect` of the process starts ChronoLog's Argobots and Margo runtime. It returns `CL_ERR_NATIVE_RUNTIME_UNSUPPORTED` (`-28`) without sending anything if Argobots or Margo is already running in the process, for example when MPI was built with MPICH `+argobots`; that refusal is permanent for the process. Do not initialize or finalize Argobots or Margo yourself, and keep the client library loaded until the process exits.

```cpp
int Client::Disconnect();
```

- Close the active session. Any stories still acquired by this client are auto-released by the Visor as part of disconnect, so callers don't need to walk their own acquisitions to clean up.
- Returns `CL_SUCCESS` on success, error code otherwise.
- Every `Client` in a process shares one context: one engine, one RPC runtime with two slots, and one RPC policy. Deleting a `Client` does not wait for remote work. When the last `Client` of a connected context is deleted, the context sends one `Disconnect` on its own, with a fresh `control_ms` budget, once its outstanding work has settled. If that `Disconnect` is not confirmed, the context stays and the process cannot create a new one. A `Client` constructed while that cleanup runs (up to one `control_ms`) gets the closing context, and its calls return `CL_ERR_RPC_CAPACITY` for its whole life.

```cpp
int Client::SetRpcTimeoutPolicy(RpcTimeoutPolicy const& policy);
```

- Set the context's budgets (see [RPC Deadlines](../../user-guide/configuration/network-and-rpc.md#rpc-deadlines)); by default they come from the `RpcPolicy` block of the client configuration file.
- Call it before the first `Connect`: entering `Connect` freezes the policy for the context's life, even if `Connect` fails. Returns `CL_ERR_INVALID_ARG` for a value of 0 or out of range, and `CL_ERR_RPC_CAPACITY` when work is already admitted.

Every metadata call below (`Connect`, `Disconnect`, chronicle and story create, acquire, release and destroy, `ShowChronicles`, `ShowStories`) has one `control_ms` budget. It returns `CL_ERR_DEADLINE_EXCEEDED` if the budget ran out before the request was sent, and `CL_ERR_OUTCOME_UNCONFIRMED` if the request was sent and not answered in time. The change may then have taken effect. Call an acquire or release again to settle it; any other change to that name is refused until then (see [Unconfirmed metadata changes](./error-codes.md#unconfirmed-metadata-changes)).

```cpp
ClientId Client::client_id() const;
```

- Returns this client's packed `ClientId` — the same value that appears in `Event::client_id()` and `EventSequence::clientId` for every event produced by this client and surfaced at retrieval.
- Valid only after a successful `Connect()`; returns `0` before that.

### Chronicle Management

```cpp
int Client::CreateChronicle(std::string const& chronicle_name);
```

- Create a new chronicle named `chronicle_name`.
- Returns `CL_SUCCESS` on success, `CL_ERR_CHRONICLE_EXISTS` if a chronicle with that name already exists, or another error code on failure.

```cpp
int Client::DestroyChronicle(std::string const& chronicle_name);
```

- Permanently delete the chronicle, including all of its stories and any persisted archive data.
- Returns `CL_SUCCESS` on success, `CL_ERR_ACQUIRED` if any story within the chronicle is currently acquired (by any client), or another error code on failure.

```cpp
std::pair<int, std::vector<std::string>> Client::ShowChronicles();
```

- Returns a pair `{return_code, names}`. On success, `return_code` is `CL_SUCCESS` and `names` contains the names of all chronicles visible to this client. On failure, `return_code` is an error code and `names` is empty.

### Story Management

```cpp
std::pair<int, StoryHandle*> Client::AcquireStory(
    std::string const& chronicle_name,
    std::string const& story_name);
```

- Open the story `story_name` inside `chronicle_name` for data access. If the story does not exist it is created.
- Returns a pair `{return_code, handle}`. On success, `return_code` is `CL_SUCCESS` and `handle` is a valid pointer; on failure, `handle` is `nullptr`.
- The `StoryHandle` is owned by the `Client` and must not be deleted by the caller.
- Returns `CL_ERR_NOT_EXIST` if the chronicle does not exist.

```cpp
int Client::ReleaseStory(std::string const& chronicle_name,
                         std::string const& story_name);
```

- Release the story handle obtained from `AcquireStory`. Pending events buffered by this client are flushed before the call returns, so a successful `ReleaseStory` guarantees that everything written via `log_event` is visible to subsequent readers.
- Returns `CL_SUCCESS` on success, `CL_ERR_NOT_ACQUIRED` if the story was not acquired by this client.

```cpp
int Client::DestroyStory(std::string const& chronicle_name,
                         std::string const& story_name);
```

- Permanently delete the story, including any persisted archive data.
- Returns `CL_SUCCESS` on success, `CL_ERR_ACQUIRED` if the story is currently acquired (by any client), or another error code on failure.

```cpp
std::pair<int, std::vector<std::string>> Client::ShowStories(
    std::string const& chronicle_name);
```

- Returns a pair `{return_code, names}`. On success, `return_code` is `CL_SUCCESS` and `names` contains the names of all stories in `chronicle_name`. On failure (e.g. `CL_ERR_NOT_EXIST` if the chronicle does not exist), `names` is empty.

### Event Replay

```cpp
int Client::ReplayStory(std::string const& chronicle_name,
                        std::string const& story_name,
                        uint64_t start,
                        uint64_t end,
                        std::vector<Event>& event_series);
```

- Retrieve all events in the story whose timestamps fall within the half-open range `[start, end)`.
- Events are appended to `event_series` in chronological order.
- Returns `CL_SUCCESS` on success, `CL_ERR_NOT_READER_MODE` if the client was constructed without a `ClientQueryServiceConf`, `CL_ERR_QUERY_TIMED_OUT` if the query did not complete in time, or another error code on failure.
- One `replay_ms` budget (180 s by default) covers the whole call: admission, lookup, the request and the player's answer. Each RPC within it is capped at `attempt_ms`. When the budget runs out the call returns `CL_ERR_QUERY_TIMED_OUT`, and nothing is written to `event_series` after that; if it ran out before the query was admitted it can return `CL_ERR_DEADLINE_EXCEEDED`. At most two replays run at a time per process; a third returns `CL_ERR_RPC_CAPACITY`.
- Returns `CL_ERR_PARTIAL_RESULT` (`-18`) when ChronoPlayer answered but knows the series is short: a keeper of the story was not fetched, or did not answer, within the player's fetch deadline (its `attempt_ms`, 5 s, for all the story's keepers, fetched two at a time), a keeper's answer hit its per-query event cap, or an archive file in the range could not be read. `event_series` holds the events it did gather; retry the range to pick up the rest.

```cpp
using EventCallback = std::function<void(Event const&)>;

int Client::ReplayStory(std::string const& chronicle_name,
                        std::string const& story_name,
                        uint64_t start,
                        uint64_t end,
                        EventCallback callback);
```

Streaming overload: invoke `callback` once per event in `[start, end)`, with no client-side `std::vector<Event>` materialization.

- The callback runs on the query service receive thread; the caller is responsible for any locking it needs. No new callback starts once the budget has run out; one already running must finish before the call returns.
- The callback must not throw. If it does, the exception is caught and logged, that event is skipped, and the remaining events in the response are still delivered.
- Scope note: this overload removes the client-side vector materialization, but the wire-level response is unchanged — the Player still ships one `PlaybackQueryResponse` per query and events are fully deserialized into a server-supplied buffer before the callback loop runs. Player-side chunked delivery is tracked as a follow-up.
- Returns the same error codes as the vector overload.

---

## `StoryHandle` Class

`StoryHandle` is obtained from `Client::AcquireStory` and provides write access as well as low-latency on-demand tail reading for a story. It is owned by the `Client` and must not be deleted by the caller.

```cpp
uint64_t StoryHandle::log_event(std::string const& event_record);
```

- Append a new event with the log record `event_record`, within one `append_ms` budget (5 s by
  default).
- Returns the `uint64_t` timestamp assigned to the event by the client library once the keeper
  acknowledged it, and `0` otherwise: the keeper refused it, the budget ran out, no RPC slot was
  free, or the keeper did not answer in time. Up to 3.5.0 it returned the timestamp whatever the
  keeper answered.
- A `0` does not always mean the event is lost: an append that was sent and not answered may have
  been stored. Use `append_event` below to tell the cases apart.
- A keeper crash before ChronoGrapher writes the event loses it without the caller being told. Use
  the overload below with `Durability::Durable` when the call must not return until the event is
  on the keeper's disk.
- Timestamps are strictly increasing per client process, also across a step back of the system
  clock.

```cpp
enum class AppendOutcome { Acknowledged, NotSubmitted, Rejected, Unconfirmed };

struct AppendResult
{
    int status;           // CL_SUCCESS, or why the event was not acknowledged
    AppendOutcome outcome;
    uint64_t timestamp;   // the event's timestamp; kept with Unconfirmed
    uint32_t event_index; // the event's index within this client
};

AppendResult StoryHandle::append_event(std::string const& event_record);
```

- The same write as `log_event(event_record)`, returning why it was or was not acknowledged:
  - `Acknowledged`, `CL_SUCCESS`: the keeper holds the event in memory.
  - `Rejected`: the keeper answered with an error (`status`), and did not store the event.
  - `NotSubmitted`: nothing was sent. `status` is `CL_ERR_DEADLINE_EXCEEDED`,
    `CL_ERR_RPC_CAPACITY`, `CL_ERR_NOT_ACQUIRED`, `CL_ERR_CLOCK_UNAVAILABLE` or
    `CL_ERR_TIMESTAMP_EXHAUSTED`.
  - `Unconfirmed`, `CL_ERR_OUTCOME_UNCONFIRMED`: the append was sent and not answered in time; the
    keeper may have stored it. `timestamp` and `event_index` identify the event, so the caller can
    look for it later. Never send it again as if the keeper had refused it: a new call creates a
    second event.
- "Acknowledged" means accepted into the keeper's memory, not written to the archive or a disk.

```cpp
enum class Durability : uint8_t
{
    Accepted = 1, // in keeper memory; what log_event(record) gives
    Durable = 2   // fsync'd in the keeper's WAL
};

struct LogEventOptions
{
    Durability durability = Durability::Accepted;
    // durable writes only: deadline per RPC attempt, and how many attempts
    // to send before giving up; an attempt that gets no answer is retried
    uint32_t timeout_ms = 10000;
    uint32_t attempts = 3;
};

int StoryHandle::log_event(std::string const& event_record,
                           LogEventOptions const& options,
                           uint64_t& event_time);
```

Append a new event and return `CL_SUCCESS` once it has reached `options.durability`. On success
`event_time` receives the event's timestamp. It also receives it with `CL_ERR_DURABILITY_UNKNOWN`,
so the caller can check later whether the event was stored; on any other return the client
library's handle sets it to `0`. The default `StoryHandle` implementation, for subclasses, returns
`CL_ERR_DURABILITY_UNAVAILABLE` to Durable without touching `event_time`.

- **`Durability::Accepted`** (the default) takes the same path as `append_event(event_record)`,
  under the same `append_ms` budget, and returns its `status`. `CL_SUCCESS` means the keeper holds
  the event in memory; a keeper that is shutting down returns `CL_ERR_UNKNOWN` and stores nothing.
  There is no retry. Only `CL_SUCCESS` sets `event_time`; with `CL_ERR_OUTCOME_UNCONFIRMED` use
  `append_event` to keep the timestamp.
- **`Durability::Durable`** returns `CL_SUCCESS` only after the keeper wrote the event to its
  write-ahead log (WAL) and `fdatasync` returned. What that survives depends on where the WAL is:
  on a local disk, a keeper crash and a node reboot; on tmpfs, a keeper crash only; it never
  survives losing the disk. See [Keeper WAL](../../user-guide/architecture/keeper-wal.md). A keeper
  that cannot make the event durable answers with an error and does not store it, except in one
  case, when the keeper cannot erase a group whose write or `fdatasync` failed and answers
  `CL_ERR_DURABILITY_UNKNOWN`; see
  [Durability error codes](./error-codes.md#durability-error-codes). It never downgrades the write
  to Accepted.
- The default stays Accepted, so existing callers keep their cost: a durable write pays one RPC per event
  and waits for a sync. Concurrent durable writes to one keeper share a sync, but a single writer
  waits for one per event.
- The WAL is off by default on the keepers. A deployment must turn it on (`--wal` in the deploy
  scripts) before a durable write can succeed; with it off every durable write gets
  `CL_ERR_DURABILITY_UNAVAILABLE`.

**Budget and slot.** One budget of `attempts × timeout_ms` starts at the call and covers admission,
lookup, every attempt and the pauses between them. The whole write holds one of the client's two RPC
slots; with no slot free it returns `CL_ERR_RPC_CAPACITY` and sends nothing.

**Retries.** A durable write builds the event once and sends it to the keeper chosen by its
timestamp, with a deadline of `timeout_ms`. When an attempt gets no answer (a timeout or a transport
error), the client sends the identical event again, up to `attempts` times, with no backoff besides
the wait below. A timed-out attempt has already used its `timeout_ms`; one that failed sooner, such
as a forward to a dead keeper, waits until `timeout_ms` has passed, so a keeper restarted at the
same address can answer a later attempt. The retry budget is therefore `attempts × timeout_ms`, 30 s
with the defaults, and the call can block that long. Every attempt carries the same identity,
`(time, client id, index)`. The keeper choice depends only on the timestamp, so a retry reaches the
same keeper while the story's keeper list is unchanged, and the keeper answers a retry of an event
it already holds with `CL_SUCCESS` without storing it again, also after a crash and restart. The
keeper holds the event until the grapher confirms it written, which is at least about a minute after
the event's timestamp, so the default retry budget always finds it. A budget longer than that can
send a retry after the confirmation; the keeper then stores the event again, and the copies collapse
on read as below.

**Storage is at-least-once; reads see each event once.** If a keeper left the story's keeper list
between attempts, the retry lands on another keeper and both may store the event. Copies with the
same identity collapse in the keeper's chunk, in `playback` and in `ReplayStory`, so a reader sees
the event once. A crash can also leave a second copy of some events in the HDF5 archive; reads drop
it the same way.

**Return values for `Durability::Durable`:**

- `CL_SUCCESS` (`0`): the event is in the keeper's WAL and synced.
- `CL_ERR_INVALID_ARG` (`-2`): nothing was sent or nothing was stored. The client refuses an empty
  record, `attempts` or `timeout_ms` of 0, and a `durability` value other than the two above. Only
  the last check applies to Accepted writes, which ignore `attempts` and `timeout_ms`. The keeper refuses an
  event larger than its WAL record limit (256 MiB minus 36 bytes).
- `CL_ERR_NO_KEEPERS` (`-7`): the story has no keepers. Nothing was sent.
- `CL_ERR_RPC_CAPACITY` (`-24`): no RPC slot was free. Nothing was sent.
- `CL_ERR_DURABILITY_UNAVAILABLE` (`-19`), `CL_ERR_DURABILITY_FULL` (`-20`),
  `CL_ERR_DURABILITY_UNKNOWN` (`-21`): see [Durability error codes](./error-codes.md#durability-error-codes).

**Subclasses and ABI.** `log_event(record, options, event_time)` is a new virtual of `StoryHandle`.
Its default implementation sends Accepted through `log_event(record)` and answers
`CL_ERR_DURABILITY_UNAVAILABLE` to Durable, sending nothing. A subclass that overrides only one of
the two `log_event` overloads adds `using StoryHandle::log_event;` so it does not hide the other.
The new virtual changes the client library's ABI: code built against an earlier `libchronolog_client`
that calls `log_event`, including the plugins, needs a rebuild but no source change.
`CLIENT_PROTOCOL_VERSION` is unchanged; a durable write uses a new keeper RPC, which an older
keeper does not register. The client returns `CL_ERR_DURABILITY_UNAVAILABLE` for the resulting
`HG_NO_MATCH` only while that keeper has never answered a durable call from this client and still
answers a tail read of no story; otherwise it counts as an unanswered attempt (see [Durability error codes](./error-codes.md#durability-error-codes)).

```cpp
chronolog::LogEventOptions options;
options.durability = chronolog::Durability::Durable;

uint64_t event_time = 0;
int ret = story->log_event("sensor reading 42", options, event_time);
if(ret == chronolog::CL_SUCCESS)
{
    // on the keeper's disk; event_time is its timestamp
}
else if(ret == chronolog::CL_ERR_DURABILITY_FULL)
{
    // nothing stored; back off and call log_event again
}
else if(ret == chronolog::CL_ERR_DURABILITY_UNKNOWN)
{
    // may be stored; calling log_event again creates a second event.
    // event_time is the event's timestamp: replay the story around it to
    // see whether the event is there
}
else
{
    // nothing stored (-19, -2, -7): retrying will not help
    std::cerr << chronolog::to_string_client(ret) << "\n";
}
```

```cpp
int StoryHandle::playback(size_t n, std::vector<Event>& events);
```

- **On-Demand Tail Read**: Retrieve the most recent `n` events of this story directly from ChronoKeeper in-memory buffers (`KeeperChunkRetentionStore`), bypassing the ChronoPlayer/HDF5 archive path.
- **Two-Phase Scatter/Gather**: Scatters sequence queries across all assigned keepers, selects the global last-$n$, and fetches only winning payloads over the wire. The keepers are asked in waves of as many as the client has free RPC slots (two), so a story with more than two keepers is read two at a time. Both phases share one `tail_ms` budget (5 s by default).
- **Ordering**: Events in `events` are populated in ascending `EventSequence` order (`(chrono_time, clientId, index)`).
- **Writer-Mode Supported**: Works with clients instantiated with `ClientPortalServiceConf` only (no `ClientQueryServiceConf` needed).
- **Return Values**:
  - `CL_SUCCESS` (`0`): Tail read succeeded. If the story has fewer than `n` events in keeper memory, returns all available tail events.
  - `CL_ERR_NO_KEEPERS` (`-7`): No keepers are assigned to this story.
  - `CL_ERR_QUERY_TIMED_OUT` (`-12`): Keeper RPC timed out during phase 1 or phase 2.
  - `CL_ERR_RPC_CAPACITY` (`-24`): no RPC slot was free; nothing was sent.
  - `CL_ERR_PARTIAL_RESULT` (`-18`): Promised events were evicted from keeper memory between phase 1 and phase 2 due to capacity limits. `events` contains the events that were successfully retrieved.


---

## `Event` Class

`Event` is the smallest data unit in ChronoLog. Events are returned by `Client::ReplayStory`.

### Constructor

```cpp
Event::Event(chrono_time event_time = 0,
             ClientId    client_id  = 0,
             chrono_index index     = 0,
             std::string const& record = std::string());
```

### Getters

```cpp
uint64_t           Event::time()       const; // event timestamp
ClientId const&    Event::client_id()  const; // originating client ID
uint32_t           Event::index()      const; // per-client sequence index
std::string const& Event::log_record() const; // the log record string
EventSequence      Event::sequence()   const; // {time, client_id, index}
```

### Operators

```cpp
bool Event::operator==(Event const& other) const; // equal if time, client_id, and index match
bool Event::operator!=(Event const& other) const;
bool Event::operator< (Event const& other) const; // ordered by time, then client_id, then index
```

### Utilities

```cpp
std::string Event::to_string() const;
```

- Returns a string in the format: `{Event:<eventTime>:<clientId>:<eventIndex>:<logRecord>}`

---

## `ClientIdentity`

Helper for packing and unpacking the 64-bit `ClientId` surfaced on every `Event`.

```cpp
struct ClientIdentity
{
    uint32_t ip = 0;       // IPv4 address in host byte order (0 if undetermined)
    uint16_t port = 0;     // query service port for reader-mode clients; 0 for writer-only
    uint16_t instance = 0; // instance discriminator (pid & 0xFFFF)

    ClientId pack() const;
    static ClientIdentity unpack(ClientId id);
};
```

`ClientId` layout: bits `[63:32]` ip, bits `[31:16]` port, bits `[15:0]` instance. The high 48 bits identify the client's network endpoint so a downstream consumer reading back events can identify the producer; the low 16 bits disambiguate multiple writer-only processes running on the same host. For a reader-mode client the ip is the address its query service listens on, unless that is a loopback address. Otherwise, and for a writer-only client, it is this host's address on the route to the visor when the visor is on another host, and the address the host name resolves to when it is not.

---

## Error Codes

See [Error Codes](./error-codes.md) for the full list of `ClientErrorCode` values returned by the methods above.
