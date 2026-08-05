# Archive manifest on NFS — validation results

The manifest was designed, built and tested on a local filesystem. Five of its
mechanisms make assumptions that are true on ext4/xfs and **not guaranteed on NFS**,
and one of them became load-bearing only when the distributed layout put two
Graphers on one manifest. This document listed what to test; it now records what
the tests found.

**Status: tested 2026-08-05 on the ares cluster. Assumptions 1 and 2 fail.**
They are no longer hypothetical risks — both are reproduced, with a diagnosis and
a confirmed fix direction. Assumptions 3, 4 and 5 hold.

| # | Assumption | Verdict | Measured |
| --- | --- | --- | --- |
| 1 | concurrent `O_APPEND` is atomic | **FAIL** | 66.1% record loss synthetic; 3.2% in a live two-Grapher run |
| 2 | `stat` reflects recent writes promptly | **FAIL** | 120.1 s detection lag, not the 1 s poll interval |
| 3 | compaction is detectable | **PASS** | detected via mtime; late, never lost |
| 4 | `link()` fails rather than clobbers | **PASS** | `EEXIST`, target inode preserved |
| 5 | `readdir` sees recently created files | **PASS** | no persistent `UNRECORDED` across restart |

The fix is **option 2 — per-Grapher manifests** (see [Recommendation](#recommendation)).
The evidence for it is stronger than "multi-writer is risky": the failure is
*exclusively cross-client*, and per-Grapher manifests make every writer single-client.

---

## Test environment

Everything below was run on `ares-comp-[03-08]` (Slurm job 22778) against
`/mnt/common`, which is where `~` and the archive directory live:

```
172.25.1.1:/mnt/common on /mnt/common type nfs
  (rw,vers=3,rsize=1048576,wsize=1048576,
   acregmin=120,acregmax=120,acdirmin=120,acdirmax=120,
   hard,proto=tcp,nconnect=8,timeo=600,retrans=2,local_lock=none)
```

Two properties of this mount dominate every result:

- **NFSv3**, not v4. There is no atomic-append primitive in the protocol.
- **`acregmin=acregmax=acdirmin=acdirmax=120`** — a flat **120-second** attribute
  cache. The original version of this document guessed "commonly 3–30 s for files,
  up to 60 s for directories". The real mount is 2–4× worse than that worst case,
  and it is the single biggest amplifier of both failures.

`local_lock=none` means advisory locks go to the server, which is why the `flock`
mitigation works (see 1a variants).

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
archive directory is normally NFS, and — as measured below — it does not.

---

## Assumption 1 — concurrent `O_APPEND` is atomic — **FAIL**

`ArchiveManifest::append` (`src/chrono-common/src/ArchiveManifest.cpp`) takes a
**process-local** mutex and then appends with `std::ofstream(path, std::ios::app)`
followed by `flush()`. Within one process that mutex serialises writers. Across
processes, correctness rests entirely on the kernel making an `O_APPEND` write
atomic.

On a local filesystem it is: the offset is resolved and the write applied under the
inode lock. **On NFS it is not**, and the failure is far from rare.

### Test 1a — bare filesystem probe

6 nodes, 2000 appends each to one shared file, synchronised to a common start second
via a spin-wait on `date +%s`.

| Variant | Expected | Got | Loss |
| --- | --- | --- | --- |
| 6 nodes, tight loop | 12000 | 4071 | **66.1%** |
| 6 nodes, 200 ms apart | 600 | 261 | **56.5%** |
| **6 procs on 1 node** | 12000 | **12000** | **0%** |
| 6 nodes + `flock` | 3000 | **3000** | **0%** |
| 6 nodes, `manifest_fsync` emulated | 6000 | 2146 | **64.2%** |

The probe is a faithful proxy: `echo >>` opens with `O_APPEND`, writes once and
closes, which is exactly what `ofstream(app)` + `flush()` + destructor does for a
~200-byte line. This was confirmed by reading `ArchiveManifest::append` rather than
assumed.

Surviving lines were all well-formed — the loss is invisible from inside the file.
But the file was not clean:

```
first NUL at byte offset: 30 of 132637
context before: b'{"node":"ares-comp-05","i":1}\n'
NUL run length: 30
context after : b'{"node":"ares-comp-04","i":1}\n...'
```

A **30-byte NUL run exactly where one 30-byte record belonged**. The file was
extended to hold the record but the data never landed. Across the run: 475 NUL bytes
and 40 blank lines. So a lost append does not always vanish silently — sometimes it
leaves a hole that a strict JSON check would see as a torn line.

### Test 1b — the real thing, two Graphers

Deployed from binaries built off this branch: Visor c03, **Grapher c04 (RG1) +
Grapher c05 (RG2) on different nodes sharing one NFS archive directory**, Keepers
c06/c07, Players c03/c08. `story_chunk_duration_secs` lowered to 15 to get a useful
number of chunks; 13.1 MB written across 8 long-lived stories.

```
manifest lines              : 182
unparsable / torn lines     : 0
NUL bytes in manifest       : 0
published records           : 91
deleted records             : 90
files the Graphers created  : 94        <- ground truth, from Grapher logs
UNRECORDED (created, never recorded as published) : 3     (3.2%)
MISSING (recorded, never on disk)                 : 0
```

The three lost records are not ambiguous. Each names a file that has a **`deleted`
record but no `published` record**:

```
chronicle_5_0.story_5_0.1785963915.vlen.h5   records: ['deleted']   <- c04
chronicle_6_0.story_6_0.1785963930.vlen.h5   records: ['deleted']   <- c04
chronicle_7_0.story_7_0.1785963870.vlen.h5   records: ['deleted']   <- c05
```

A file cannot be deleted without first having been published. The Grapher appended a
`published` record, NFS dropped it, and the later `deleted` append for the same file
survived. Both Graphers lost records, so this is not one misbehaving node.

Note the manifest itself is **intact and fully parseable** — 182/182 lines valid, no
NULs. Every integrity check the original plan proposed passes. Only the
cross-reference against what the Graphers actually did reveals the loss. Any future
check for this must compare against external ground truth; the file cannot detect
its own missing records.

---

## Assumption 2 — `stat` reflects recent writes promptly — **FAIL**

`HDF5ArchiveReadingAgent::pollManifestTail` decides there is new data by comparing
`fs::file_size` and `fs::last_write_time` against what it applied last.

### Test 2

Writer on c03 appended 30 manifest-sized records, 1 s apart (done by t≈33 s). A
poller on c04 replicated `pollManifestTail`'s detection exactly — `stat()` the path
every 250 ms, compare size and mtime:

```
t=   0.00s  size=0
t= 120.14s  size=5400      <- all 30 records appear at once
```

**120.14 seconds**, matching `acregmax=120` precisely, against a
`manifest_poll_interval_ms` of 1000.

### Diagnosis

The original text suspected the attribute cache but expected "tens of seconds" and
noted the poller "re-opens the log each tick, so close-to-open consistency gives it
fresh bytes". The measurement shows why that reasoning does not rescue detection:

```cpp
auto const size  = fs::file_size(log_path, ec);        // stat() on the PATH
auto const mtime = fs::last_write_time(log_path, ec);  // stat() on the PATH
...                                                     // decide there is new data
std::ifstream log(log_path, std::ios::binary);          // only NOW is it opened
```

Both calls `stat()` the **path** without opening the file. Close-to-open consistency
revalidates on `open()` — but the `open()` never happens unless the `stat()` has
already reported a change. The data path is fine; the **detection path is governed
entirely by the attribute cache**, and on this mount that is a flat 120 s.

Consequence: a Player on a different node from the Grapher is blind to newly
published archive files for up to two minutes. Replay of a recent window falls back
to the keepers' hot path — correct but slower — or returns nothing if the keepers
already released it. This is a latency failure, never a correctness one.

Mitigations: mount the archive with `actimeo=1` (or `noac`, heavier), or have the
poller `open()` the log before deciding — an `open()` forces revalidation, which
would make detection track the poll interval instead of the cache timeout. The
latter is a small change and worth considering independently of assumption 1.

---

## Assumption 3 — compaction is detectable — **PASS**

`snapshot()` writes `archive_manifest.json.tmp`, `rename`s it over the snapshot, then
**truncates the log**. The Player detects that truncation by size shrinking, or — when
the log has already been refilled to coincidentally the same length — by the mtime
having changed.

### Test 3

The dangerous case was reproduced directly: write 450 bytes, then truncate and refill
to **exactly 450 bytes** from another node, so only mtime distinguishes old from new.

```
writer c03:  t=  0.00s  initial write, size=450
             t=150.05s  truncated -> 0, refilled -> 450   (same_size_as_before: true)

poller c04:  t=  0.00s  size=  0
             t=120.14s  size=450  mtime=...097
             t=240.28s  size=450  mtime=...247    <- compaction detected via mtime
```

The `size == offset && mtime != mtime_` branch fires and the Player re-reads from 0.
This works because the server preserves **nanosecond mtime granularity** — a rewrite
50 ms later still changes mtime:

```
rewrite 0: mtime delta = 52001384 ns  (CHANGED)
```

Had the server quantised mtime to 1 s, a fast truncate-and-refill to an identical
size would have been undetectable — that would have been the "report as a bug"
outcome. It is worth knowing this passes *because of a server property*, not because
of anything the code guarantees.

Detection is still 120 s late, and because both index mutators are idempotent the
worst case is re-applying records already held. Correctness holds; only latency
suffers, and it suffers the same 120 s as assumption 2.

---

## Assumption 4 — `link()` fails rather than clobbers — **PASS**

`StoryChunkWriter::publishFile` publishes an archive file with `link()` precisely
because it fails with `EEXIST` instead of replacing an existing file.

### Test 4

```
$ python3 -c "import os;os.link('src','taken')"
FileExistsError: [Errno 17] File exists: 'src' -> 'taken'

taken inode 32724437574   (unchanged)
src   inode 32724437573
```

`EEXIST` as required, and the target keeps its own inode — no clobber. As expected:
this is one of the oldest NFS guarantees, and it holds here.

---

## Assumption 5 — `readdir` sees recently created files — **PASS**

`reconcileManifestWithDirectory()` lists the archive directory to find files the
manifest does not name. No separate test was needed: the three `UNRECORDED` entries
from Test 1b are explained by lost appends (each has a `deleted` record proving the
file was known), not by directory-cache staleness, and none persisted as phantom
entries. `acdirmax=120` still applies, so a reconciliation pass running within two
minutes of another node creating a file may miss it — self-correcting at the next
Grapher start.

---

## Diagnosis — why assumption 1 fails, and what actually fixes it

**The failure is cross-client, not concurrent-writer.** This is the central finding
and it is what makes the fix cheap to reason about:

- 6 processes on **one node** → **0 loss out of 12000**
- 6 processes on **six nodes** → **66% loss out of 12000**

One NFS client resolves the append offset once and serialises its own writers through
the inode; within a client, `O_APPEND` behaves exactly as it does on ext4. Across
clients there is nothing to serialise them. NFSv3's `WRITE` carries an **explicit
offset** — there is no append opcode — so each client computes EOF itself and two
clients readily compute the same one. The second write lands on top of the first.

The 120 s attribute cache makes this much worse than a narrow race: a client's notion
of EOF can be two minutes stale, so it is not that writers occasionally collide, it is
that they can be persistently wrong about where the file ends. That is why the
zero-filled holes appear — a client writing at an offset derived from a stale size
leaves a gap the server zero-fills.

**Loss scales with how often appends overlap.** The synthetic tests (6 writers at
30–2000 appends/s) lost 56–66%; the live two-Grapher run (2 writers, ~91 records over
4 minutes, well under 1 append/s) lost 3.2%. Sparse appends collide less often. But
3.2% is not "rare" in the sense the original document was willing to tolerate — the
bar it set was *"a steady stream during normal operation means `O_APPEND` is not
holding"*, and three losses in a four-minute run is a steady stream.

**`manifest_fsync` does not help, as predicted.** 64.2% loss with it versus 66.1%
without — indistinguishable. It flushes data to the server; it does nothing about
offset resolution. The prediction in the original plan was correct and the knob should
not be presented as a mitigation for this.

**`flock` does work here** — 3000/3000, zero loss, ~25 s for 3000 locked appends
across 6 nodes (≈8 ms each, including forking `flock` and a shell per iteration; the
in-process cost would be lower). `local_lock=none` sends locks to the server and
NFSv3 `lockd` is functioning on this mount. It is a smaller change than restructuring
the manifest, but it adds a network round trip per published chunk and depends on
`lockd` being healthy — widely distrusted on NFSv3 for good reason.

### Recommendation

**Option 2 — give each Grapher its own manifest.** Make the archive directory (or at
least the manifest filename) per recording group, and have the Player load and merge
every `archive_manifest*.log` it finds. Readers already tolerate duplicate records
across sources, so merging is additive.

The same-node result is the argument: single-client appends lose nothing, so removing
the multi-writer-across-nodes property removes the failure mode outright rather than
narrowing its window. `flock` (option 3) narrows the window at a per-chunk cost;
`manifest_fsync` (option 1) does nothing; accepting it (option 4) leaves Players
missing files until the next Grapher restart.

Independently, consider making `pollManifestTail` `open()` the log before deciding
whether it changed. That addresses assumption 2, is a few lines, and is orthogonal to
the manifest layout.

---

## Reproducing

Probe scripts are in `/mnt/common/kfeng/nfs_manifest_probe/`:

| Script | What it measures |
| --- | --- |
| `probe.sh` | 1a tight-loop concurrent append (synchronised start) |
| `probe_rate.sh` | 1a at a realistic publish cadence |
| `probe_local.sh` | same-node multi-process control |
| `probe_flock.sh` | `flock`-serialised append (option 3) |
| `probe_fsync.sh` | `manifest_fsync` emulation (option 1) |
| `stat_poller.py` / `stat_writer.py` | assumption 2 detection lag |
| `compaction_writer.py` | assumption 3 truncate-and-refill |

Read the manifest with `dd iflag=direct` when checking results from a node that did
not write it — a cached `stat`/read can be up to 120 s stale and will misreport
counts.

Two operational notes for whoever runs this next:

- **Cluster deploys could not start Keepers** until `8e34054f`. `f02a2c6e` removed
  `csv_tier_extractor` from the keeper conf template but left the `jq` line in
  `deploy_cluster.sh` that writes into it, producing an extractor with no `"type"`;
  `ExtractionModuleConfiguration` rejects it and every keeper exits at startup. Fixed
  on this branch by the one-line removal in `8e34054f`. `origin/watermark-feedback`
  still carries the breakage.
- **`chrono-bench -t` is `--story_count`, not threads** (CLAUDE.md is wrong on this),
  and `perf_bench.cpp` computes `story_count / chronicle_count` with integer division.
  `-t 1 -h 4` therefore yields zero stories, writes nothing, and still prints a
  throughput table. Keep `-t` ≥ `-h`. Stories must also outlive
  `story_chunk_duration_secs` or the Grapher discards their chunks as orphans
  ("Refusing to adopt orphan chunk for destroyed StoryId") and every manifest record
  comes out `state:"empty"`.

---

## Resolution — what was implemented

Both failing assumptions were addressed, along the lines the recommendation above
argued for. Nothing here narrows a race window; both changes remove the mechanism
that produced the failure.

### Assumption 1 — one manifest log per Grapher

`ArchiveManifest` takes a writer id and derives its own file names from it:

| | before | after |
| --- | --- | --- |
| log | `archive_manifest.log` | `archive_manifest.<group>.log` |
| snapshot | `archive_manifest.json` | `archive_manifest.<group>.json` |

The writer id is the **recording group id** (`GRAPHER_CONF.RECORDING_GROUP`). It has
to be stable across restarts — a pid would leave every restart's log stranded under
a name nothing writes to or compacts again — and the recording group is.

Every log is now written by exactly one client, which is the configuration that
measured **0% loss** in test 1a (6 procs on 1 node). The 66% figure came from 6
procs on 6 *nodes*; the split makes that arrangement unreachable rather than
unlikely.

Readers merge. `ArchiveManifest::load()` globs `archive_manifest*.log` plus each
one's snapshot and reads them all, so:

- the Player's startup index covers every Grapher's output;
- the Grapher's own startup load still sees the whole archive, which reconciliation
  needs or it would adopt another Grapher's files as orphans and record them twice;
- an archive written before the split still loads — the unsuffixed pair matches the
  same glob, and a manifest constructed without a writer id keeps using it.

Compaction stays private to the writer. `snapshot()` writes back only the records
that came from *this* writer's log or snapshot, tracked separately from the merged
set. Compacting the merged set instead would copy another writer's records into our
snapshot while their log still holds them, duplicating a record on every compaction.

### Assumption 2 — open before deciding

`pollManifestTail` used to `stat()` the path, compare size and mtime against what it
had already applied, and return early if unchanged — opening the file only after
deciding something had changed. Over NFS the early return is self-perpetuating: a
client revalidates cached attributes on **open**, and a `stat()` of the path is
answered from the attribute cache, so a poll that never opens the file never
refreshes the cache it is reading. That is the 120.14 s observed in test 2, matching
the mount's `acregmax=120` exactly.

The poll now opens the log first, takes the size from the open stream, and only then
decides. The `stat()` for mtime stays, but it now runs against attributes the open
just revalidated.

The poll also follows several logs at once, with a cursor per log, and re-lists the
directory each tick so a Grapher that starts after the Player is still picked up.

### Verification

Local (`Debug`, ext4):

- `chronolog-test-common-archive-manifest` — 33 tests, all pass. 6 cover the split:
  separate logs per writer, merged reads, compaction isolation (including after a
  merged load), legacy-only archives, legacy and per-writer coexisting.
- `chronolog-test-player-archive-index` — 17 tests, all pass. 3 new: index built
  from every Grapher's manifest, one poll draining every tail with independent
  offsets, a log created after startup being noticed.
- Mutation-checked. Ignoring the writer id, dropping the cross-writer merge,
  compacting the merged set, polling only a shared log, and sharing one offset
  across logs each fail at least one test.
- `tests/integration/manifest_restart_test.sh` — 9/9. The deployment writes
  `archive_manifest.1.log` and the restarted Player indexes from it with no scan.
- `watermark_loop_test.sh`, `watermark_replay_split_test.sh` — all assertions pass.

Both watermark scripts now delete the manifest along with the HDF5 files they
already removed. They did not before, so a second run started against a manifest
describing files that were gone, restored a stale watermark, and failed the
retention assertions for an unrelated reason.

**Still to confirm on the cluster.** Local runs cannot exercise either fix: ext4 has
no attribute cache to go stale and no cross-client writers. Re-run the probes in
*Reproducing* below against this build. What should change:

- test 1b (two Graphers, live): expect 0 lost records, and two logs in the archive
  dir instead of one.
- test 2 (detection lag): expect the lag to drop from ~120 s to about the poll
  interval (`manifest_poll_interval_ms`, default 1000).
