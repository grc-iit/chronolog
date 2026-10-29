---
sidebar_position: 9
title: "Keeper WAL and Durable Writes"
---

# Keeper WAL and Durable Writes

A durable write returns `CL_SUCCESS` only after the ChronoKeeper has written the event to its
write-ahead log (WAL) and `fdatasync` returned. If the keeper then crashes, it reads the WAL when it
starts again and sends the events ChronoGrapher has not confirmed through the usual path.

Without the WAL, an event reaches the HDF5 archive 60 to 90 seconds after its timestamp, and until
then it exists only in keeper memory (see [Durable Chunk Retention](./durable-retention.md)). A
keeper crash in that interval loses it, and the writer is never told.

The WAL is off by default. With it off, a keeper behaves as in 3.3.0. With it on and no durable
writes, a keeper still prepares two 64 MiB segments, writes a `D` record for every story destroyed,
writes an `exit-summary` as it exits and logs a stats line every 10 seconds. The grapher syncs every
window it writes in both cases (see [Grapher fsync](#grapher-fsync)).
Only durable writes go into the WAL: an accepted write, `log_event(record)` or
`Durability::Accepted`, takes the same path and costs the same as before. The client API is in
[C++ API](../../client/cpp/api.md#storyhandle-class) and [Python API](../../client/python/api.md#storyhandle-class).

---

## Write path

The client sends a durable write to the keeper chosen by the event's timestamp, through a separate
RPC, `record_event_durable`, that also carries the chronicle and story names. The keeper:

1. answers `CL_ERR_DURABILITY_UNAVAILABLE` if its WAL is off;
2. answers `CL_ERR_INVALID_ARG` if the event's story id does not match the names;
3. looks up the event's identity, `(time, client id, index)`, in the WAL. If the event is already
   synced and not yet released, it answers `CL_SUCCESS` and stores nothing; if an earlier attempt is
   still waiting for its sync, it waits for that attempt's result. This is how a client's retry gets
   the first attempt's answer before any later check can refuse it; only the WAL-off and story-id
   checks come first;
4. answers `CL_ERR_INVALID_ARG` to an empty record, `CL_ERR_DURABILITY_UNAVAILABLE` if its WAL has
   failed, and `CL_ERR_DURABILITY_FULL` if the WAL's file system is below its reserve or the keeper
   holds more than `durable_cap_mb` of chunks;
5. starts a pipeline for the story if it has none, as `start_story_recording` would. It retires one
   acceptance window later unless the visor starts the story;
6. queues the event and waits. A committer on its own thread takes everything queued, up to
   `group_commit_max_bytes`, writes it with one `writev` and syncs it with one `fdatasync`, so
   concurrent durable writes share a sync;
7. after the sync, puts the event into the ingestion queue like any accepted event, and answers
   `CL_SUCCESS`. From here on the event is sealed, shipped, archived, tail-read and replayed exactly
   as an accepted event is.

If the write or the sync fails, none of the group is ingested and the WAL stays failed until the
keeper restarts. Accepted writes continue. After a failed `fdatasync`, Linux may mark the dirty
pages clean and keep them, and a later sync then returns 0 without writing them. So before it
answers any record of the group, the keeper overwrites the bytes the group was written to with
zeros and syncs them. Zeros end a segment's data, so a keeper restarted on the same boot reads the
group back as no records and ships none of its events.

- When the zeros are written and synced, every writer in the group gets
  `CL_ERR_DURABILITY_UNAVAILABLE`. The events are not on disk.
- When the zeros cannot be written or synced, the records may be on disk, and every writer in the
  group gets `CL_ERR_DURABILITY_UNKNOWN`. This includes a failed write that may have written
  nothing: the keeper cannot tell, so it also erases after a write error and answers -21 if the
  erase fails.
- A segment switch that fails before any of the group is written has nothing to erase, and the group
  gets `CL_ERR_DURABILITY_UNAVAILABLE`.

One case remains: the keeper crashes after the failed write or sync, before the zeros are synced.
The writers got no answer at all, so they see -21. If a retry reaches the restarted keeper within
the client's retry window, the keeper may have read the group back from the page cache, the retry finds
the event, and the writer sees `CL_SUCCESS`, although the group may not be on the disk. A restart
on the same boot can read the group back and ship it.

A handler waiting for its group yields its Argobots thread, so the keeper's RPC threads keep serving
other requests, and a group can hold more writes than the keeper has RPC threads.

---

## WAL files

Each keeper owns one directory:

- `lock`, held with `flock` for the life of the process, so two keepers cannot share a directory;
- numbered segments, `00000000000000000001.wal` and on, each `segment_bytes` long (64 MiB);
- `<n>.wal.partial`, a segment being prepared, removed at startup;
- `exit-summary`, written as the keeper exits, with the number of durable events the grapher had not
  yet confirmed.

A segment is written full of zeros, given its header and synced before it is used, so a write
overwrites zeros and a sync does not also commit a change of file size. That cut the sync time 1.7
times on the Ares boot SSD and 2 to 4.5 times on the other drives measured. The keeper keeps one such spare ready and switches to it when
the active segment is full. The thread that prepares the spare sleeps while it has nothing to do, so a keeper with the WAL on and no writes uses about 1 % of a core.

Each record is a frame with its length and a CRC32C. The records are a segment header (`H`, with a
format version), a story's names (`N`, once per story per segment), an event (`E`) and a story
dropped (`D`). An event record holds at most 256 MiB.

---

## Release

A WAL record goes the first time the retention store finds the event's chunk confirmed written:
the grapher acknowledged the chunk, its receipt is settled, and the persisted watermark covers it
(see [Durable Chunk Retention](./durable-retention.md#how-chunks-are-kept-and-freed)). Release does
not wait until the chunk is freed, which can take much longer while the tail index still holds it.

Events are also released when the pipeline discards them, and when the grapher reports their story
destroyed: the keeper then writes a `D` record, and once it is synced the story's earlier records no
longer count. A `D` record in a group that failed is erased with the group, like any other
record of it. The story's earlier events can then come back after a restart, as they do after a
power loss before the `D` record synced; a grapher that still holds the story's tombstone refuses
them.

A segment is deleted, oldest first, once none of its events is pending. A healthy keeper holds about
two minutes of durable writes, from the write to the grapher's confirmation, and the WAL directory
usually holds the active segment and a spare.

The WAL never drops an event the grapher has not confirmed. Above `max_bytes` (1 GiB) it logs a
warning, since that means the grapher is behind or down. Durable writes are refused with
`CL_ERR_DURABILITY_FULL` while the free space on the WAL's file system is below the reserve, the
larger of `reserve_min_bytes` and `reserve_percent` of the file system, or while the keeper holds
more than `durable_cap_mb` of chunks in memory.

---

## Recovery

A keeper with the WAL on reads it at startup, before it starts its recording service and before it
registers with the visor:

1. It takes the lock. If another process holds it, the keeper exits and names the directory.
2. It removes `*.partial` and checks every segment's header. A segment from a build with another WAL
   format stops startup with the segment and both versions; nothing is truncated.
3. It reads every record and checks its length and CRC. In the newest segment that holds data, the
   first bad record is a write the crash cut short: it ends the data, and the rest of the segment is
   zeroed. A bad record in any older segment stops startup with the segment and offset; move that
   segment aside to start without it.
4. It syncs every segment and the directory, starts writing in a fresh segment, and rebuilds the
   table of pending events, so a client retry of an event synced before the crash gets `CL_SUCCESS`
   without a second copy.
5. It splits the recovered events into sealed chunks aligned to `story_chunk_duration_secs`, and the
   normal extraction path sends them to the grapher. The grapher usually has no pipeline for those
   stories and adopts the chunks by their chronicle and story names. Its confirmations come back to
   this keeper and release the records.

The keeper logs what it replayed: the segments, each story with its event count and time range, the
bytes and the time taken. Recovery reads the whole WAL, so startup takes longer the more it holds.

Some recovered events were already written before the crash; their segment survived because
another event in it was still pending. They are written to the archive a second time, and replay
returns each event once.

A restarted keeper registers with the visor again, retrying every 5 seconds while the visor still
holds its old entry. The visor then starts a pipeline on it for every story its recording group's
clients still hold, so accepted writes after the restart are kept too.

---

## Grapher fsync

A keeper drops its WAL records when the grapher reports a window written, so the grapher makes the
window durable first. Before it advances the watermark or settles a receipt, it syncs the window
file, its directory after the rename, and the manifest record that names the file; the first time it
writes into a story directory it also syncs the chronicle directory and the archive root. This
applies to every window, with durable events or not, so accepted events also survive a power loss
on the grapher node once written.

- A failed sync of a window file counts as a failed write: the file is removed and the keepers send
  the chunks again.
- A failed sync of the manifest stops all watermark advances and receipt settlements until the
  grapher restarts, so keepers and their WALs keep holding the events.

Measured on a workstation NVMe with 1,000 stories, writing a window took 1.85 ms on average in a
burst of writes, against 0.27 ms in 3.3.0 without the syncs.

---

## Failure behavior

| Situation | What happens |
|---|---|
| Keeper crashes after the sync, before answering | The client gets no answer and sends the same event again. Once the keeper is back, recovery has put the event in its pending table, and the retry gets `CL_SUCCESS` with no second copy. If the retries run out first, the client returns `CL_ERR_DURABILITY_UNKNOWN`; the event is stored and is shipped once the keeper restarts. |
| Keeper killed or node rebooted, WAL on a local disk | Every durable event that got `CL_SUCCESS` and was not yet confirmed written is replayed when a keeper starts again on the same directory. Until then, events only that keeper held are not readable. |
| WAL on tmpfs | Survives a keeper crash, not a reboot. |
| WAL write or sync fails | The keeper zeros the group's bytes and syncs them, then answers the group `CL_ERR_DURABILITY_UNAVAILABLE`. If it cannot erase the group, it answers `CL_ERR_DURABILITY_UNKNOWN`. The WAL stays failed until a restart; accepted writes continue. |
| Grapher down for a long time | The WAL and keeper memory grow; a warning above `max_bytes`; durable writes refused at `durable_cap_mb` or the reserve. Nothing is dropped. |
| Torn record at the end of the newest segment | Zeroed at startup. |
| Bad record in an older segment | The keeper refuses to start and names the segment and offset. |
| WAL from a build with another format | The keeper refuses to start and names the segment and both versions. |
| WAL directory locked | The keeper exits and names the directory. |
| Keeper exits with unconfirmed durable events | It logs how many and writes them to `exit-summary`. `deploy_cluster.sh --stop` reads that file; a missing one means the keeper crashed. |
| Story destroyed | The keeper writes a `D` record and releases the story's events. A grapher that restarted after the destroy, or a later job on the same node, can bring the story's events back from a WAL replay. So can a `D` record in a group whose write or sync failed, which is erased; a grapher that still holds the story's tombstone refuses them. |

---

## Configuration

The WAL is the `WAL` block of the `chrono_keeper` section; see
[Server Configuration](../configuration/server-configuration.md#wal--keeper-write-ahead-log). The
deploy scripts turn it on with `--wal` and choose the directory on each keeper host; see
[Multi-Node Deployment](../deployment/multi-node.md#keeper-wal).

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Turns the WAL on. |
| `dir` | `""` | The WAL directory, absolute. Required when enabled. |
| `segment_bytes` | `67108864` | Size of a segment, at least 1 MiB. |
| `max_bytes` | `1073741824` | WAL size above which the keeper warns. Nothing is dropped. |
| `group_commit_max_bytes` | `4194304` | Most bytes written and synced in one group. |
| `group_commit_window_us` | `0` | How long the committer waits to grow a group; `0` syncs as soon as the previous sync returns. At most `10000`. |
| `reserve_min_bytes`, `reserve_percent` | `268435456`, `10` | Free space kept on the WAL's file system: the larger of the two. Below it, durable writes get `CL_ERR_DURABILITY_FULL`. |
| `durable_cap_mb` | `4096` | Chunk memory above which durable writes get `CL_ERR_DURABILITY_FULL`. |

Every 10 seconds a keeper with the WAL on logs a `[KeeperWal] stats` line at INFO level: groups,
events and bytes written, the largest group, pending events and bytes, and segments.
