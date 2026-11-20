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
| `CL_ERR_DURABILITY_UNAVAILABLE` | -19 | Durable write: the keeper has no WAL, its WAL failed, it is shutting down, or it lacks the durable RPC. Nothing was stored |
| `CL_ERR_DURABILITY_FULL` | -20 | Durable write: the keeper's WAL file system is below its reserve, or the keeper holds more unconfirmed data than `durable_cap_mb`. Nothing was stored |
| `CL_ERR_DURABILITY_UNKNOWN` | -21 | Durable write: the event may be stored. `event_time` holds its timestamp (see [Durability error codes](#durability-error-codes)) |
| `CL_ERR_DEADLINE_EXCEEDED` | -22 | The call's budget ran out before its request was sent. Nothing was sent (see [Deadline and capacity codes](#deadline-and-capacity-codes)) |
| `CL_ERR_OUTCOME_UNCONFIRMED` | -23 | The request was sent, but no answer came within the budget. The operation may have taken effect |
| `CL_ERR_RPC_CAPACITY` | -24 | Every RPC slot of the client is taken, or the client is closing. Nothing was sent |
| `CL_ERR_CLOCK_UNAVAILABLE` | -25 | The event time could not be read. Nothing was sent |
| `CL_ERR_TIMESTAMP_EXHAUSTED` | -26 | No timestamp larger than the client's last one can be assigned. Nothing was sent |
| `CL_ERR_SHUTDOWN_INCOMPLETE` | -27 | A stop ran out of its budget while work it owns was still running |
| `CL_ERR_NATIVE_RUNTIME_UNSUPPORTED` | -28 | `Connect`: Argobots or Margo was already initialized in this process, for example by an MPICH built with `+argobots`. Permanent for the process; nothing was sent |

:::note Reserved range
Codes `-14` through `-17` are intentionally unassigned. They are reserved for the ingest acceptance-window and collision-policy work developing in parallel, so that branch and this one never assign the same number to different meanings.
:::

## Durability error codes

Only `StoryHandle::log_event(record, options, event_time)` with `Durability::Durable` returns these
three codes (see [the API reference](./api.md#storyhandle-class)). Each says whether the event can
be stored, which decides what the caller does next.

| Code | Was the event stored? | Cause | What to do |
|------|-----------------------|-------|------------|
| `CL_ERR_DURABILITY_UNAVAILABLE` (-19) | No | The keeper runs with the WAL off; its WAL hit a write or `fdatasync` error and stays failed until the keeper restarts; the keeper is shutting down; or the keeper is older than 3.4.0 and lacks the durable RPC. | Retrying will not help until an operator acts: turn the WAL on, restart the keeper whose WAL failed (its log has a critical line), wait until a keeper that is shutting down has been restarted, or upgrade the keepers. The client returns it at once, with no retry. If losing the event on a keeper crash is acceptable, write it with `Durability::Accepted`; the client never does that by itself. |
| `CL_ERR_DURABILITY_FULL` (-20) | No | The free space on the keeper's WAL file system is below the reserve (the larger of `reserve_min_bytes` and `reserve_percent` of the file system), or the keeper holds more than `durable_cap_mb` of chunks the grapher has not confirmed. Usually ChronoGrapher is behind or down, so the keeper can release nothing. | Retry later, with a backoff. The keeper samples its free space every 100 ms and admits durable writes again once the space is back above the reserve and the unconfirmed chunks are down to three quarters of `durable_cap_mb`. A chunk stops counting when the grapher confirms it, even while the tail index still holds it. |
| `CL_ERR_DURABILITY_UNKNOWN` (-21) | Maybe | No attempt got an answer within `timeout_ms`, after `attempts` attempts. Also returned when an earlier attempt went unanswered and a later one got -19, -20 or found no durable RPC, and when the story's keeper list became empty between attempts: the unanswered attempt may have reached a keeper that stored the event. And when the keeper answers -21 itself: its WAL failed while writing the event and could not erase the bytes it wrote, so they may be on disk. | The caller decides. A new `log_event` call creates a second event, with a new timestamp and index, so if both are stored a reader sees two events. To make that safe, put an identifier of your own in the record and drop repeats when reading. To find out whether the event was stored, use `event_time`; see below. |

A durable write that gets -19 or -20 was not ingested either: the event does not appear in
`playback` or in a replay. With -21 it may appear, once.

When a group's write or `fdatasync` fails, the keeper overwrites the group's bytes with zeros and
syncs them before it answers, so a keeper restarted on the same boot does not read the group back.
Every writer in the group then gets -19. When the keeper cannot erase the group, it answers -21,
because the records may be on disk. It cannot tell a write that failed before writing anything from
one that wrote part of the group, so a failed write whose erase also fails gets -21 as well. If the
keeper crashes after the failed write or sync, before the zeros are synced, the writers got no
answer at all and see -21. If a retry reaches the restarted keeper within the client's retry window
(3 attempts of 10 s by default), the keeper may have read the group back from the page cache, the
retry finds the event, and the writer sees `CL_SUCCESS`, although the group may not be on the disk.

### Checking whether a -21 event was stored

With -21, `event_time` is the event's timestamp, and with every other failure it is 0. The
timestamp is the first part of the event's identity. To check whether the event was stored, wait
until a restarted keeper has replayed its WAL, then replay the story over a short
range around `event_time` with `ReplayStory` and look for a record that matches yours. Put an
identifier of your own in the record to make the match certain. If a matching record is there,
the event was stored; do not write it again. If none is there after the keepers have recovered,
write it again with a new `log_event` call.

### Retries and shutdown

A keeper that is shutting down (SIGTERM) takes no new write once its shutdown starts, after it has
unregistered from the visor: it answers a durable write -19 and an accepted write `CL_ERR_UNKNOWN`
(-1), and stores nothing. Durable writes it had already queued are synced and answered as usual. If
an earlier attempt of the same write went unanswered, the writer sees -21 instead of -19, as with
any -19.

This lasts until the keeper deletes its recording service, late in the shutdown. That deregisters
all its RPCs, including the tail read the client uses as a probe, so a durable write is then an
unanswered attempt as against a dead keeper: the client waits, retries, and returns -21. An
accepted write still gets -1.

The client reads "RPC not registered" from a keeper as "this keeper lacks the durable RPC" only
while that keeper has never answered a durable call from this client, and only if the keeper still
answers a tail read of no story, an RPC every keeper since 3.1 defines. Over `ofi+sockets` a call to
a keeper that is down fails with the same error, but then the tail read gets no answer either, so
the call is an unanswered attempt and is retried, as is every call after the keeper's first durable
answer. A call that fails at once is not retried at once: the client waits until the attempt's
`timeout_ms` has passed, so the attempts are `timeout_ms` apart and a keeper restarted at the same
address can answer. If it does not come back in time, the writer gets -21 with the event's
timestamp.

## Deadline and capacity codes

Since 3.6.0 every client call has a budget from the client's [`RpcPolicy`](../../user-guide/configuration/network-and-rpc.md#rpc-deadlines) (`append_ms`, `control_ms`, `tail_ms`, `replay_ms`). What a caller does next depends on whether the request left the process:

| Code | Was the request sent? | What to do |
|------|-----------------------|------------|
| `CL_ERR_DEADLINE_EXCEEDED` (-22) | No | Retrying is safe. The budget ran out while the call waited for a slot, looked up the server or prepared the request. |
| `CL_ERR_OUTCOME_UNCONFIRMED` (-23) | Yes | Do not treat it as a failure. An accepted write may be stored: use `append_event`, which returns the event's timestamp with this code, and check for the event as for a [-21](#checking-whether-a--21-event-was-stored). A metadata change may have taken effect; see [Unconfirmed metadata changes](#unconfirmed-metadata-changes). |
| `CL_ERR_RPC_CAPACITY` (-24) | No | Retry after a short backoff. The client has two RPC slots. A slot whose call got no answer stays taken until Mercury is done with it, so a server that stopped answering can hold both, and every later call returns -24 at once. A `Client` constructed while the previous last `Client` of the process is still closing gets -24 for its whole life. |
| `CL_ERR_CLOCK_UNAVAILABLE` (-25), `CL_ERR_TIMESTAMP_EXHAUSTED` (-26) | No | The client could not give the event a timestamp. Timestamps are strictly increasing per client, also across a clock step back; -26 means the clock is at the largest value. |
| `CL_ERR_NATIVE_RUNTIME_UNSUPPORTED` (-28) | No | ChronoLog must initialize the process's Argobots and Margo itself. Build MPI applications against an MPICH without Argobots (`~argobots`, as the shipped Spack environment does), and do not initialize Argobots or Margo before `Connect`. |

### Unconfirmed metadata changes

After a metadata change returns -23, the client refuses, also with -23, every later change that names the same story, or its chronicle, until the outcome is settled. Repeating the change could otherwise apply it twice. While the first call's work is still in flight, which can outlast its budget, the refusal comes back at once: retry after a short backoff.

- **`AcquireStory` or `ReleaseStory` returned -23.** Call it again. The client sends a repeated acquire, or a release, of the story to ChronoVisor, whose answer settles the outcome. ChronoVisor answers a repeated acquire by the client that already holds the story with the story's current keepers, so the call returns a handle. A release returns `CL_SUCCESS` once this client no longer holds the story, whether the earlier call took effect or not. After a release that returned -23, an acquire of that story stays refused until a release succeeds, because the client still holds the earlier handle. A call again that ends before it is sent, for example because its budget ran out while it waited, returns -23 and leaves the outcome as it was.
- **`Disconnect`.** It is sent while the only unsettled changes are acquires and releases. ChronoVisor releases every story the client holds, and a confirmed `Disconnect` settles them all; destroying the last `Client` sends one the same way.
- **A create or destroy returned -23.** The name stays refused for the life of the process, and so do `Disconnect` and a replacement `Client`. Check with `ShowChronicles` or `ShowStories` whether the change took effect, then restart the process.

ChronoVisor itself settles a story whose start or stop it could not confirm on the next acquire or release of that story, by any client.

`log_event(record)` returns the event's timestamp, or 0 for any failure, so it cannot tell these cases apart. `append_event(record)` returns an `AppendResult` with the code (`status`), the `outcome` (`Acknowledged`, `NotSubmitted`, `Rejected` or `Unconfirmed`), and the `timestamp` and `event_index` it gave the event. With `Unconfirmed` the timestamp is kept, so the event can be looked up; never send an `Unconfirmed` event again as if the keeper had refused it. A keeper's own refusal comes back with `Rejected` and the keeper's code.

## Error Handling

Use `to_string_client(int)` to get the error name as a string for logging:

```cpp
int ret = client.Connect();
if (ret != chronolog::CL_SUCCESS) {
    std::cerr << "Connect failed: " << chronolog::to_string_client(ret) << "\n";
    return 1;
}
```
