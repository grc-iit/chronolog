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

```cpp
int Client::Disconnect();
```

- Close the active session. Any stories still acquired by this client are auto-released by the Visor as part of disconnect, so callers don't need to walk their own acquisitions to clean up.
- The destructor calls `Disconnect()` as best-effort; the `Client` is always safe to delete.
- Returns `CL_SUCCESS` on success, error code otherwise.

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

```cpp
using EventCallback = std::function<void(Event const&)>;

int Client::ReplayStory(std::string const& chronicle_name,
                        std::string const& story_name,
                        uint64_t start,
                        uint64_t end,
                        EventCallback callback);
```

Streaming overload: invoke `callback` once per event in `[start, end)`, with no client-side `std::vector<Event>` materialization.

- The callback runs on the query service receive thread; the caller is responsible for any locking it needs.
- The callback must not throw. If it does, the exception is caught and logged, that event is skipped, and the remaining events in the response are still delivered.
- Scope note: this overload removes the client-side vector materialization, but the wire-level response is unchanged — the Player still ships one `PlaybackQueryResponse` per query and events are fully deserialized into a server-supplied buffer before the callback loop runs. Player-side chunked delivery is tracked as a follow-up.
- Returns the same error codes as the vector overload.

---

## `StoryHandle` Class

`StoryHandle` is obtained from `Client::AcquireStory` and provides write access to a story. It is owned by the `Client` and must not be deleted by the caller.

```cpp
uint64_t StoryHandle::log_event(std::string const& event_record);
```

- Append a new event with the log record `event_record`.
- Returns the `uint64_t` timestamp assigned to the event by the client library.

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

`ClientId` layout: bits `[63:32]` ip, bits `[31:16]` port, bits `[15:0]` instance. The high 48 bits identify the client's network endpoint so a downstream consumer reading back events can identify the producer; the low 16 bits disambiguate multiple writer-only processes running on the same host.

---

## Error Codes

See [Error Codes](./error-codes.md) for the full list of `ClientErrorCode` values returned by the methods above.
