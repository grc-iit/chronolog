---
sidebar_position: 3
title: "Error Codes"
---

# Error Codes

All Client API methods return an `int` status code. Check the return value against `chronolog::CL_SUCCESS` (0) before proceeding — any negative value indicates an error. All codes are defined in the `chronolog` namespace.

```cpp
#include <client_errcode.h>
```

## Client Error Codes

| Code | Value | Description |
|------|------:|-------------|
| `CL_SUCCESS` | 0 | Operation completed successfully |
| `CL_ERR_UNKNOWN` | -1 | Generic/unexpected error |
| `CL_ERR_INVALID_ARG` | -2 | Invalid argument or parameter |
| `CL_ERR_NOT_EXIST` | -3 | Chronicle or Story does not exist |
| `CL_ERR_ACQUIRED` | -4 | Resource is acquired and cannot be destroyed |
| `CL_ERR_NOT_ACQUIRED` | -5 | Resource is not acquired and cannot be released |
| `CL_ERR_CHRONICLE_EXISTS` | -6 | Chronicle with that name already exists |
| `CL_ERR_NO_KEEPERS` | -7 | No ChronoKeeper nodes available |
| `CL_ERR_NO_CONNECTION` | -8 | No connection to ChronoVisor |
| `CL_ERR_NOT_AUTHORIZED` | -9 | Operation not authorized |
| `CL_ERR_NO_PLAYERS` | -10 | No ChronoPlayer nodes available |
| `CL_ERR_NOT_READER_MODE` | -11 | Client is in WRITER_MODE; read operations unavailable |
| `CL_ERR_QUERY_TIMED_OUT` | -12 | Replay query or tail-read RPC timed out |
| `CL_ERR_PROTOCOL_VERSION_MISMATCH` | -13 | Client and server disagree on the wire-protocol version |
| `CL_ERR_PARTIAL_RESULT` | -18 | Query returned fewer records than it should: for a tail read, events left keeper memory between its two phases; for a replay, a keeper missed its fetch deadline or hit its event cap, or an archive file could not be read. The records that came back are valid |
| `CL_ERR_DURABILITY_UNAVAILABLE` | -19 | Durable write: the keeper has no WAL, its WAL failed, or it lacks the durable RPC. Nothing was stored |
| `CL_ERR_DURABILITY_FULL` | -20 | Durable write: the keeper's WAL file system is below its reserve, or the keeper holds more data than `durable_cap_mb`. Nothing was stored |
| `CL_ERR_DURABILITY_UNKNOWN` | -21 | Durable write: no attempt got an answer that settles it. The event may be stored |

:::note Reserved range
Codes `-14` through `-17` are intentionally unassigned. They are reserved for the ingest acceptance-window and collision-policy work developing in parallel, so that branch and this one never assign the same number to different meanings.
:::

## Durability error codes

Only `StoryHandle::log_event(record, options, event_time)` with `Durability::Durable` returns these
three codes (see [the API reference](./api.md#storyhandle-class)). Each says whether the event can
be stored, which decides what the caller does next.

| Code | Was the event stored? | Cause | What to do |
|------|-----------------------|-------|------------|
| `CL_ERR_DURABILITY_UNAVAILABLE` (-19) | No | The keeper runs with the WAL off; its WAL hit a write or `fdatasync` error and stays failed until the keeper restarts; or the keeper is older than 3.4.0 and lacks the durable RPC. | Retrying will not help until an operator acts: turn the WAL on, restart the keeper whose WAL failed (its log has a critical line), or upgrade the keepers. The client returns it at once, with no retry. If losing the event on a keeper crash is acceptable, write it with `Durability::Accepted`; the client never does that by itself. |
| `CL_ERR_DURABILITY_FULL` (-20) | No | The free space on the keeper's WAL file system is below the reserve (the larger of `reserve_min_bytes` and `reserve_percent` of the file system), or the keeper holds more than `durable_cap_mb` of chunks in memory. Usually ChronoGrapher is behind or down, so the keeper can release nothing. | Retry later, with a backoff. The keeper samples its free space every 100 ms and admits durable writes again once the space is back above the reserve and the grapher has confirmed enough chunks written. |
| `CL_ERR_DURABILITY_UNKNOWN` (-21) | Maybe | No attempt got an answer within `timeout_ms`, after `attempts` attempts. Also returned when an earlier attempt went unanswered and a later one got -19, -20 or found no durable RPC, and when the story's keeper list became empty between attempts: the unanswered attempt may have reached a keeper that stored the event. | The caller decides. A new `log_event` call creates a second event, with a new timestamp and index, so if both are stored a reader sees two events. To make that safe, put an identifier of your own in the record and drop repeats when reading. |

A durable write that gets -19 or -20 was not ingested either: the event does not appear in
`playback` or in a replay. With -21 it may appear, once.

The client reads "RPC not registered" from a keeper as "this keeper lacks the durable RPC" only
while that keeper has never answered a durable call from this client; after one answer it is an
unanswered attempt and is retried. Over `ofi+sockets` a call to a keeper that is down fails with
the same error, so a client's first durable call to a dead keeper also gets -19, with no retry. One
case of this is not settled: if a keeper dies with a client's first durable request in flight and
the transport reports it that way, the client returns -19 although the keeper may have synced the
event.

## Error Handling

Use `to_string_client(int)` to get the error name as a string for logging:

```cpp
int ret = client.Connect();
if (ret != chronolog::CL_SUCCESS) {
    std::cerr << "Connect failed: " << chronolog::to_string_client(ret) << "\n";
    return 1;
}
```
