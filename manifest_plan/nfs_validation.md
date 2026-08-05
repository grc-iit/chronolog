# Archive manifest on NFS — what to validate before trusting it there

The manifest was designed, built and tested on a local filesystem. Three of its
mechanisms make assumptions that are true on ext4/xfs and **not guaranteed on NFS**,
and one of them became load-bearing only when the distributed layout put two
Graphers on one manifest. This is the list of what to test on a real NFS mount,
what "pass" looks like, and what to do if a test fails.

Nothing here is a known bug. It is a list of assumptions that have not been checked
on the filesystem the cluster actually uses.

**Before running tests 2 and 3**, set the Player's log level to `debug` — the lines
they grep for (`Applied N manifest record(s)`, `Manifest log shrank`, `Manifest log
rewritten`) are all `LOG_DEBUG`, so at `info` the greps return nothing and the tests
look like failures. The local deployment already runs the Player at `debug`; a
cluster deployment may not.

---

## Why this matters now

`tools/deploy/single_user_deploy.sh` places components by host file, and the
distributed CI layout puts **two Graphers** (c4, c5) on **one archive directory**.
`deploy_local.sh` likewise sets `hdf5_archive_dir` without indexing it per recording
group. So in any multi-recording-group deployment:

- N Grapher processes append to **one** `archive_manifest.log`
- M Player processes read that same file concurrently
- all of them through whatever filesystem the archive directory lives on

On one node that is a local filesystem and the assumptions hold. On a cluster the
archive directory is normally NFS, and it does not.

---

## Assumption 1 — concurrent `O_APPEND` is atomic (**highest risk**)

`ArchiveManifest::append` (`src/chrono-common/src/ArchiveManifest.cpp`) takes a
**process-local** mutex and then appends with `std::ofstream(path, std::ios::app)`
followed by `flush()`. Within one process that mutex serialises writers. Across
processes, correctness rests entirely on the kernel making an `O_APPEND` write
atomic.

On a local filesystem it is: the offset is resolved and the write applied under the
inode lock. **On NFS it is not.** The client resolves the append offset itself, so
two clients can compute the same offset and the second write lands on top of the
first. The failure mode is not interleaved text — it is a **silently lost record**.

A lost record is the same failure the design already treats as survivable (W
under-reports, keepers retain longer and re-send, `E <= W` still holds, and
reconciliation adopts an orphaned file at the next Grapher start). So this is a
degradation, not corruption. But if it happens at any rate above "rare", the
manifest stops being the cheap authority it is meant to be and every Player start
leans on reconciliation instead.

### Test 1a — bare filesystem probe (no ChronoLog needed)

Run this **on the NFS mount that will hold the archive directory**, ideally from
two or more different cluster nodes at once.

```bash
# On each of N nodes, against the SAME shared path:
SHARED=/path/on/nfs/append_probe
: > "$SHARED/log"          # once, from one node only

# then on every node simultaneously:
NODE=$(hostname -s)
for i in $(seq 1 2000); do
  echo "{\"node\":\"$NODE\",\"i\":$i}" >> "$SHARED/log"
done
```

Pass: `wc -l < "$SHARED/log"` equals `2000 * <number of nodes>`, and

```bash
sort "$SHARED/log" | uniq -d        # expect no output
grep -cv '^{.*}$' "$SHARED/log"     # expect 0 (no torn lines)
```

Fail: fewer lines than expected (**lost appends** — assumption 1 is violated), or
lines that are not well-formed JSON objects (**torn writes**, which the loader
discards, so they show up as loss too).

Note `echo >>` is a faithful proxy: it opens with `O_APPEND` and writes once, which
is what `ofstream` + `flush()` does for a ~200-byte line.

### Test 1b — the real thing, two Graphers

Deploy with **two recording groups** so two Graphers share the archive directory,
write for a few minutes, then stop cleanly and check:

```bash
ARCHIVE=/path/to/archive
# every line parses
grep -cv '^{.*}$' "$ARCHIVE/archive_manifest.log"     # expect 0
# every published record names a file that exists
grep '"state":"published"' "$ARCHIVE/archive_manifest.log" |
  sed -n 's/.*"file":"\([^"]*\)".*/\1/p' | sort -u |
  while read -r f; do [ -f "$ARCHIVE/$f" ] || echo "MISSING $f"; done
# every archive file is named by some record  <-- the loss detector
ls "$ARCHIVE"/*.h5 2>/dev/null | xargs -n1 basename |
  while read -r f; do
    grep -q "\"file\":\"$f\"" "$ARCHIVE/archive_manifest.log" || echo "UNRECORDED $f"
  done
```

Pass: no `MISSING`, no `UNRECORDED`, no unparsable lines.

`UNRECORDED` output is the signal that matters. It means a file was published whose
record was lost — exactly what assumption 1 protects against, and exactly what
`reconcileManifestWithDirectory()` will adopt at the next Grapher start. A handful
after an unclean shutdown is expected and fine. A steady stream during normal
operation means `O_APPEND` is not holding.

### If it fails

In increasing order of cost:

1. **Turn on `manifest_fsync`** (`chrono_grapher.DataStoreInternals.manifest_fsync`).
   This does **not** fix lost appends — it flushes to the server, it does not make
   the offset resolution atomic. Try it only to see whether it changes the rate;
   do not treat it as the fix.
2. **Give each Grapher its own manifest.** The cleanest fix, and it removes the
   multi-writer assumption entirely: make the archive directory (or at least the
   manifest name) per recording group, and have the Player load and merge all
   `archive_manifest*.log` it finds. Readers already tolerate duplicate records
   across sources, so merging is additive. This is a real change, not a knob.
3. **Serialise appends with an advisory lock.** `flock` on NFSv4 works; on NFSv3 it
   needs `lockd` and is widely distrusted. Adds a network round trip per published
   chunk.
4. **Accept it and lean on reconciliation.** Already implemented and idempotent: the
   Grapher adopts unrecorded files at startup, so the manifest converges. The cost
   is that Players started between the loss and the next Grapher restart miss those
   files, and W stays low until the keeper re-sends.

Option 2 is what I would do if Test 1 fails.

---

## Assumption 2 — `stat` reflects recent writes promptly

`HDF5ArchiveReadingAgent::pollManifestTail` decides there is new data by comparing
`fs::file_size` and `fs::last_write_time` against what it applied last. Both are
`stat` results, and NFS caches attributes (`acregmin`/`acregmax`, commonly 3–30 s for
files, up to 60 s for directories). The **data** is fine — the poller re-opens the
log each tick, so close-to-open consistency gives it fresh bytes — but it may not
*notice* there are bytes to read.

Consequence: a Player can lag behind newly published files by the attribute cache
timeout instead of `manifest_poll_interval_ms` (default 1000). Replay of a very
recent window would fall back to the keepers' hot path, which is correct but
slower, or return nothing if the keepers already released it.

### Test 2

With a Player running against an NFS archive:

```bash
# note the time, publish a window, then poll for it to become replayable
date +%s
# ... write events, wait for the grapher to archive one chunk ...
grep '"state":"published"' "$ARCHIVE/archive_manifest.log" | tail -1   # note its file
# then time how long until the player's log shows it applied
grep 'Applied .* manifest record' "$WORK_DIR/monitor/chrono-player-1.log" | tail -3
```

Pass: the gap between the record appearing in the log and the Player applying it is
on the order of `manifest_poll_interval_ms`, not tens of seconds.

Fail: consistently tens of seconds. Mitigations: mount the archive with
`actimeo=1` (or `noac`, heavier), or accept the lag — it is a latency issue, never a
correctness one.

---

## Assumption 3 — compaction is detectable

`snapshot()` writes `archive_manifest.json.tmp`, `rename`s it over the snapshot, then
**truncates the log**. The Player detects that truncation by size shrinking, or — when
the log has already been refilled to coincidentally the same length — by the mtime
having changed (`CompactionTruncatingTheLogDoesNotLoseTheIndex` covers this).

On NFS both signals come from cached attributes, so the detection can be late. It
cannot be *wrong* in a damaging direction: a missed truncation makes the Player
re-read from a stale offset, and because both index mutators are idempotent the
worst case is re-applying records it already has.

### Test 3

Force a compaction by setting `manifest_snapshot_threshold_entries` low (say 50),
publish past it, and confirm from the Player log that it keeps applying records
afterwards rather than going quiet:

```bash
grep -E 'Manifest log (shrank|rewritten)|Applied .* manifest record' \
  "$WORK_DIR/monitor/chrono-player-1.log" | tail -10
```

Pass: the Player logs the shrink/rewrite detection and keeps applying records.

Fail: the Player stops applying records after the compaction. That is the one
outcome worth reporting as a bug rather than tuning — it would mean neither signal
survived the cache.

---

## Assumption 4 — `link()` fails rather than clobbers

`StoryChunkWriter::publishFile` publishes an archive file with `link()` precisely
because it fails with `EEXIST` instead of replacing an existing file (`rename` would
replace it, and with several extraction streams that would silently destroy an
already-archived chunk).

`link()` returning `EEXIST` atomically is one of the older NFS guarantees — it was
the standard NFS locking primitive for exactly this reason — so this is expected to
hold. It is listed because it is now on the critical path for data retention, not
because it is suspected.

### Test 4

On the NFS mount:

```bash
cd /path/on/nfs && : > src && : > taken
python3 -c "import os;os.link('src','taken')" 2>&1   # expect FileExistsError
```

Pass: `FileExistsError`. Fail: it succeeds, or replaces `taken` — in which case
`publishFile` must switch to `open(O_CREAT|O_EXCL)` plus a copy, and the
non-clobbering test in `story_chunk_writer_test.cpp` will need a matching change.

---

## Assumption 5 — `readdir` sees recently created files

`reconcileManifestWithDirectory()` lists the archive directory to find files the
manifest does not name. NFS directory attribute caching can hide a file created
moments earlier by another node.

Impact is small and self-correcting: a file missed by one reconciliation pass is
adopted by the next Grapher start, and until then the keeper re-send path covers it.
No test needed beyond noticing whether `UNRECORDED` entries from Test 1b persist
across a Grapher restart. If they do, the directory cache is the likely reason and
`actimeo` tuning applies.

---

## Summary

| # | Assumption | Risk | If it fails |
| --- | --- | --- | --- |
| 1 | concurrent `O_APPEND` is atomic | **high** — silent record loss | per-Grapher manifests (option 2) |
| 2 | `stat` is prompt | medium — Player lag, not incorrect | `actimeo=1`, or accept |
| 3 | compaction is detectable | low — idempotent either way | report as a bug |
| 4 | `link()` returns `EEXIST` | low — expected to hold | `O_CREAT\|O_EXCL` + copy |
| 5 | `readdir` is prompt | low — self-correcting | `actimeo` tuning |

Test 1 is the one that decides whether the current design is usable unchanged on a
cluster. The other four are tuning or confirmation.
