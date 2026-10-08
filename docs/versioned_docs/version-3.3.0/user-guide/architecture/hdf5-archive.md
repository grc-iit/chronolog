---
sidebar_position: 8
title: "HDF5 Archive"
---

# HDF5 Archive

ChronoGrapher writes each merged story chunk to an HDF5 file in its archive directory
(`hdf5_archive_dir`), and ChronoPlayer reads those files back for replays from the same directory
(`story_files_dir`). With several recording groups, every grapher writes to the same archive, and
the archive has to be on a file system all grapher and player nodes mount; see
[Archive on a Shared File System](../deployment/multi-node.md#archive-on-a-shared-file-system).

---

## Layout

```
<archive>/
├── %manifest/
│   ├── 1.log                          one log per grapher, named after its recording group
│   └── 2.log
└── <chronicle>/
    └── <story>/
        ├── 1790713770.1.1790713500123456789.3.41.vlen.h5    the window starting at that second
        ├── 1790713770.1.1790713500123456789.3.57.vlen.h5    a later write of the same window
        └── 1790713800.1.1790713500123456789.3.44.vlen.h5
```

A file holds one window of the story: `story_chunk_duration_secs` long (30 s in the template). Its
name is `<start second>.<recording group>.<grapher start time, ns>.<incarnation>.<sequence>.vlen.h5`:
the second the window starts, the grapher that wrote it (its recording group and when that process
started), the grapher's pipeline of the story (a story destroyed and created again gets a new one),
and a number the grapher counts up for every file it writes. So no two files ever get the same name,
not even after a grapher restarts. When keeper chunks for a window arrive after the grapher has
written it (a slow keeper, or a chunk sent again), the grapher writes the window again to another
file. A replay reads every file of a window.

Chronicle and story names are written as they are, dots included, with these exceptions, which a
directory name cannot hold:

| In a name | Written as |
|---|---|
| `/` | `%2F` |
| `%` | `%25` |
| a NUL character | `%00` |
| a name that is exactly `.` or `..` | `%2E`, `%2E%2E` |
| an empty name | `%` |

Because the directories separate the names, stories whose names join to the same string (chronicle
`a.b` with story `c`, and chronicle `a` with story `b.c`) have separate files. Each name can be up to
the file system's limit for one directory name, usually 255 bytes after encoding.

A grapher writes a file under a temporary name and moves it into place when it is complete, so a
reader never opens a partial file, and two writes of the same window each get a file of their own.

---

## Archive manifest

Each grapher appends a record to its log in `%manifest/` for every file it publishes and for every
story or chronicle whose files it deletes. The log is one JSON object per line, starting with a
header that names the writer and the time the log was created:

```json
{"op":"open","writer":"1","time":1790713500234567890}
{"op":"publish","chronicle":"host01","story":"cpu_usage","file":"host01/cpu_usage/1790713770.1.1790713500123456789.3.41.vlen.h5","start":1790713770000000000,"end":1790713800000000000,"events":6}
{"op":"delete","chronicle":"host01","story":"cpu_usage","writer_start":1790713500123456789,"up_to_incarnation":3}
{"op":"delete","chronicle":"host01","whole_chronicle":true,"writer_start":1790713500123456789,"up_to_incarnation":5}
```

A deletion covers the files its log recorded before it, except those its writer process (started at
`writer_start`) wrote for a pipeline numbered above `up_to_incarnation`: those belong to the story
created again after the destroy. A deletion without those two fields covers every earlier file of
the log.

The player finds a story's files through these records and never lists the archive directories.
Before each replay it reads what the logs gained since the previous one, so a file can be replayed
as soon as its record is appended.

The grapher appends a file's record before it tells the keepers the window is written. Keepers free
their copy of a chunk only after that confirmation, plus `archive_visibility_delay_secs`, so by the
time a keeper lets go of an event, the player can find the file that holds it. A record that cannot
be appended counts as a failed write: the grapher removes the file, and the keepers keep the chunk and
send it again. See [Durable Chunk Retention](./durable-retention.md).

Each grapher has a log of its own because NFS has no atomic append: two clients appending to one file
compute the end of the file from their own caches and overwrite each other's records. A grapher opens
and closes its log for every record, so a player on another node sees the record the next time it
opens the log.

The logs only grow; a record is about 190 bytes. A player reads them in full, in 4 MiB pieces, when
it starts.

`%manifest/` is the archive's index: a player finds a file only through its record, and nothing
rebuilds a lost log. Delete, move or copy `%manifest/` only together with the story directories. A
file whose record is gone stays readable with HDF5 tools, but replays no longer return it.

---

## Destroying a story or a chronicle

Destroying a story deletes its archive files; destroying a chronicle deletes those of all its
stories. A grapher can run the destroy after the story was already created again (it first waits for
the windows still being written), and the new story writes into the same directory, so each grapher
deletes only the destroyed story's files: its own files of pipelines older than the destroy, every
file of an earlier process of its recording group, and another grapher's file only when it was last
modified more than 120 s before the destroy arrived. The other graphers run the same destroy for
their own files; the last rule covers one that is down, and the margin absorbs a difference between
the file server's clock and the grapher's. The grapher hosts and the archive's file server must keep
their clocks within those 120 s of each other (NTP): with a grapher's clock further ahead, a destroy
can delete another grapher's files of the story created again; with it further behind, a down
grapher's files of the destroyed story can stay and be replayed. A grapher that is down during a
destroy also keeps its files of the destroyed story from its last 120 s, and they are replayed once
the story is created again. The chronicle and story directories stay, and file names are never reused, so no path a
player has looked up is ever removed and created again. On NFS, a client that looked a path up while
it was missing can keep answering "not found" for it for up to `acdirmax`; a path that came back
would be hidden that long. The directories are removed with the rest of the archive when a
deployment is cleaned (`deploy_local.sh -c`, `deploy_cluster.sh -c`).

Every grapher runs the destroy, and each appends a deletion record to its own log. The player drops
the files a deletion's log recorded before it, without looking them up. A file that is gone before
the deletion's record arrives is skipped without failing the replay.

The archive directory itself may be a symlink, but no chronicle or story directory below it may be
one. A destroy that meets such a directory deletes nothing in it, records no deletion and fails;
cleaning a deployment skips it and keeps `%manifest/`. Neither follows a link out of the archive
into other data.

---

## Archives from earlier releases

Releases before 3.3.0 wrote every file into the archive directory itself, as
`<chronicle>.<story>.<start second>.vlen.h5`, without a manifest. 3.3.0 does not read or delete
those files: start a 3.3.0 deployment on a new archive directory.
