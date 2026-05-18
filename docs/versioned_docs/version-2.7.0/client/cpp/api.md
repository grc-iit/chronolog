---
sidebar_position: 2
title: "API Reference"
---

# C++ Client API

## Client Class

The `Client` class manages the connection to ChronoVisor and provides all chronicle, story, and event operations.

### Constructors

```cpp
// WRITER_MODE — event logging only
Client::Client(ClientPortalServiceConf const &portalConf)

// WRITER + READER MODE — event logging and story replay
Client::Client(ClientPortalServiceConf const &portalConf, ClientQueryServiceConf const &queryConf)
```

- The first constructor creates a write-only client connected to the ChronoVisor portal service.
- The second constructor additionally connects to the ChronoPlayer query service, enabling `ReplayStory`. Calling `ReplayStory` on a write-only client returns `CL_ERR_NOT_READER_MODE`.

### Connection

```cpp
int Client::Connect()
```

- Establish a session with ChronoVisor.
- Returns `CL_SUCCESS` on success, error code otherwise.

```cpp
int Client::Disconnect()
```

- Close the active session.
- Returns `CL_SUCCESS` on success, error code otherwise.

### Chronicle Management

```cpp
int Client::CreateChronicle(std::string const &chronicle_name,
                             std::map<std::string, std::string> const &attrs,
                             int &flags)
```

- Create a new chronicle named `chronicle_name`.
- `attrs` sets chronicle properties (e.g. tiering policy); pass an empty map for defaults.
- `flags` enables or disables optional creation flags.
- Returns `CL_SUCCESS` on success, `CL_ERR_CHRONICLE_EXISTS` if a chronicle with that name already exists, or another error code on failure.

```cpp
int Client::DestroyChronicle(std::string const &chronicle_name)
```

- Permanently delete the chronicle named `chronicle_name`.
- Returns `CL_SUCCESS` on success, `CL_ERR_ACQUIRED` if any story within the chronicle is currently acquired, or another error code on failure.

```cpp
int Client::GetChronicleAttr(std::string const &chronicle_name,
                              std::string const &key,
                              std::string &value)
```

- Retrieve the attribute `key` for the named chronicle and store the result in `value`.
- Returns `CL_SUCCESS` on success, `CL_ERR_NOT_EXIST` if the chronicle or attribute does not exist.

```cpp
int Client::EditChronicleAttr(std::string const &chronicle_name,
                               std::string const &key,
                               std::string const &value)
```

- Update the attribute `key` on the named chronicle to `value`.
- Returns `CL_SUCCESS` on success, `CL_ERR_NOT_EXIST` if the chronicle or attribute does not exist.

```cpp
std::vector<std::string> &Client::ShowChronicles(std::vector<std::string> &out)
```

- Populate `out` with the names of all chronicles visible to this client.
- Returns a reference to `out`.

### Story Management

```cpp
std::pair<int, StoryHandle *> Client::AcquireStory(
    std::string const &chronicle_name,
    std::string const &story_name,
    std::map<std::string, std::string> const &attrs,
    int &flags)
```

- Open the story `story_name` inside `chronicle_name` for data access. If the story does not exist it is created.
- Returns a `pair` where `.first` is the return code (`CL_SUCCESS` on success) and `.second` is a pointer to a `StoryHandle`. The pointer is `nullptr` on failure.
- `attrs` sets story properties; pass an empty map for defaults.
- Returns `CL_ERR_NOT_EXIST` if the chronicle does not exist.

```cpp
int Client::ReleaseStory(std::string const &chronicle_name,
                          std::string const &story_name)
```

- Release the story handle obtained from `AcquireStory`. Buffered events are flushed.
- Returns `CL_SUCCESS` on success, `CL_ERR_NOT_ACQUIRED` if the story was not acquired by this client.

```cpp
int Client::DestroyStory(std::string const &chronicle_name,
                          std::string const &story_name)
```

- Permanently delete the story.
- Returns `CL_SUCCESS` on success, `CL_ERR_ACQUIRED` if the story is currently acquired.

```cpp
std::vector<std::string> &Client::ShowStories(std::string const &chronicle_name,
                                               std::vector<std::string> &out)
```

- Populate `out` with the names of all stories in `chronicle_name`.
- Returns a reference to `out`.

### Event Replay

```cpp
int Client::ReplayStory(std::string const &chronicle_name,
                         std::string const &story_name,
                         uint64_t start,
                         uint64_t end,
                         std::vector<Event> &event_series)
```

- Retrieve all events in the story whose timestamps fall within `[start, end]`.
- Events are appended to `event_series` in chronological order.
- Returns `CL_SUCCESS` on success, `CL_ERR_NOT_READER_MODE` if the client was constructed without a `ClientQueryServiceConf`, `CL_ERR_QUERY_TIMED_OUT` if the query did not complete in time, or another error code on failure.

---

## StoryHandle Class

`StoryHandle` is obtained from `Client::AcquireStory` and provides write access to a story. It is owned by the `Client` and must not be deleted by the caller.

```cpp
uint64_t StoryHandle::log_event(std::string const &event_record)
```

- Append a new event with the log record `event_record`.
- Returns the `uint64_t` timestamp assigned to the event by the client library.

---

## Event Class

`Event` is the smallest data unit in ChronoLog. Events are returned by `Client::ReplayStory`.

### Constructor

```cpp
Event::Event(chrono_time event_time = 0,
             ClientId    client_id  = 0,
             chrono_index index     = 0,
             std::string const &record = std::string())
```

### Getters

```cpp
uint64_t         Event::time()       const  // event timestamp
ClientId const & Event::client_id()  const  // originating client ID
uint32_t         Event::index()      const  // per-client sequence index
std::string const & Event::log_record() const  // the log record string
```

### Operators

```cpp
bool Event::operator==(const Event &other) const  // equal if time, client_id, and index match
bool Event::operator!=(const Event &other) const
bool Event::operator< (const Event &other) const  // ordered by time, then client_id, then index
```

### Utilities

```cpp
std::string Event::to_string() const
```

- Returns a string in the format: `{Event:<eventTime>:<clientId>:<eventIndex>:<logRecord>}`

---

## Error Codes

See [Error Codes](./error-codes.md) for the full list of `ClientErrorCode` values returned by all methods above.
