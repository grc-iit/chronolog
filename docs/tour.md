# A one-hour tour

Three demos ship in the repository. Each runs against a real single-node instance and asserts what it claims; its
last line is `PASS <name>` or `FAIL <reason>`. After each one this page says what you just saw and where it lives in
the code. Do them in order, and read the code listed after each stop before moving on.

## Before you start

You need:

- An install prefix built as in [getting-started.md](getting-started.md#install-a-single-node), with
  `$prefix/bin` on `PATH`. Run the demos with `$prefix/bin/python`, which carries the `chronolog` module and the `mcp`
  client library.
- Two empty directories, one for the WAL and one for the local archive. Stop 1 and stop 2 each want fresh ones.
- For stop 2, one more empty directory on a second, slower file system. An NFS mount is ideal. The demo makes the tier
  unavailable with `chmod 0`, which root ignores, so run it as an ordinary user.
- For stop 3, `clio-coder` on `PATH` and a model endpoint it can reach.

Run everything from the repository root:

```sh
export PATH="$prefix/bin:$PATH"
wal=$HOME/tour/wal; archive=$HOME/tour/archive
mkdir -p "$wal" "$archive"
```

## Stop 1: agent memory

```sh
"$prefix/bin/python" deploy/demo/agent_memory.py --wal-dir "$wal" --local-root "$archive"
```

The demo starts the instance `default` and opens a context named `decisions` through the `chronolog-mcp` server, the
same one an agent talks to. Session 1 remembers five decisions, each under its own operation id, and prints one receipt
per remember:

```
remember <receipt JSON with event_id and hlc>
```

Session 2 is a separate MCP process. It recalls the whole context and then a time window of the middle three
decisions. Both pages must have `answer_complete` true and carry exactly the EventIds and HLCs of the receipts, in
order. Then the demo kills the supervisor with SIGKILL, runs `chronolog up`, and recalls again. Last it kills the Keeper
with SIGKILL, reads once while the supervisor is restarting it, and recalls again after the restart. A read during the
crash may be certified complete or not; if it claims completeness it must be right. The run ends with:

```
PASS agent-memory
```

What you just saw: a remember returned `stored: durable` only after the Keeper's WAL fsync returned (I5.6). Every
read carried a Completion, and `complete=true` meant no event of that range was missing (I6.8). After two different
crashes the same events came back with the same identities in the same order (I7.1). The agent never saw the crashes
except as a delay.

Where it lives:

- `launcher/` is the supervisor behind `chronolog up`, `status` and `down`. It restarts a dead Keeper with bounded
  backoff and, after a supervisor kill, restarts the instance on the same endpoints.
- `plugins/mcp` is the MCP server and its twelve tools. The demo asserts that exactly twelve are listed.
- `client/cpp/context` is the Context API the tools call: `context.cpp` for open and remember, `recall.cpp`, `latest.cpp`,
  `follow.cpp`, `reconcile.cpp`.
- `src/keeper/wal` holds the journal and the file sink. `WalJournal.cpp` issues the fsync that DURABLE depends on.
- `src/player/replay` merges hot and archived events into one ordered stream. `CompletionPolicy.cpp` decides the
  Completion each stream ends with.

## Stop 2: tiering

```sh
wal=$HOME/tour/wal2; archive=$HOME/tour/archive2; slow=/mnt/nfs/$USER/chronolog-tour
"$prefix/bin/python" deploy/demo/tiering.py --wal-dir "$wal" --local-root "$archive" --slow-root "$slow"
```

All three directories must be empty or absent. The demo creates an instance named `tiering` (change it with
`--instance`), registers `$slow` as a slow tier of rank 1 named `nfs`, and shortens the migration timers so the whole
sequence fits in minutes: archive windows two seconds long, files moved once three seconds old.

It writes 64 events of 64 KiB through the Python SDK and waits until every archive file is on `nfs` and none is left on
`local`. A read of the whole range must come back complete with the receipts' identities:

```
read with nfs available: 64 events, complete=True reason=<reason>
```

Then it removes all permissions from the slow root. Reads must now end incomplete with reason SOURCE_FAILED, and
`chronolog status` must report the tier unavailable:

```
read with nfs unavailable: <n> events, complete=False reason=SOURCE_FAILED
```

The scrubber keeps running during the outage. The demo reads the Grapher's log and fails if any pass recorded LOST. It
then restores the permissions and waits for a complete read, which must return the original 64 EventIds and HLCs:

```
read with nfs restored: 64 events, complete=True
PASS tiering
```

What you just saw: a file moved to a slower tier by one fsync'd manifest line, so that every reader resolves it to
exactly one location (I13.13, I13.14). A tier that cannot be reached makes its files unreadable, and a read over them
says so with SOURCE_FAILED instead of returning a short answer that claims to be whole (I6.14). Unavailable is never
recorded as lost, and the data is back, unchanged, when the tier returns (I13.15).

Where it lives:

- `src/grapher` has `MigrationWorker.cpp`, which moves files and appends the `migrate_v1` line, and the scrubber in
  `ArchiveService.cpp`, whose per-pass `archive scrub validated=... lost=... slow_failed=...` log line the demo parses.
- `src/common/tier` has `FileTierStore`, the archive store with its manifest log and tier chain, and `PosixTier`, the
  POSIX tier behind one tier root.
- `src/player` reads each archive file at its effective location and ends the stream SOURCE_FAILED when it cannot.
  `src/player/replay/CompletionPolicy.cpp` decides the Completion, SOURCE_FAILED included.
- `launcher/` implements `chronolog tier add` and the status fields the demo polls.

## Stop 3: Clio-coder with shared memory

Two Clio-coder sessions share one ChronoLog context through `chronolog-mcp`. The first session remembers three facts.
The second is a fresh process with no conversation history; it recalls the facts, complete, with the same event ids the
first session's receipts carried. Point the script at a model endpoint, then run it:

```sh
export CLIO_PROVIDER_URL=<URL of your model endpoint>
export CLIO_MODEL=<model name your endpoint serves>
bash deploy/demo/clio_memory.sh
```

What you just saw: the memory belongs to the instance, not to the agent process. A second agent, or the same agent
tomorrow, reads what the first one stored, and the Completion in each page tells it whether the answer is whole.

Where it lives: `plugins/mcp` is the server both sessions launch. The `mcp.yaml` block that registers it with
Clio-coder is in [getting-started.md](getting-started.md#agents-claude-code-codex-and-clio-coder). The Context API and
the Keeper and Player paths underneath are the ones from stop 1.

## Next

Read [architecture.md](architecture.md) for the map, then the sections of [ARCHITECTURE.md](../ARCHITECTURE.md) it
points to for the part you will work on: section 5 for acknowledgements, section 6 for Completion, section 13 for
tiers. [CONTRIBUTING.md](../CONTRIBUTING.md) has the build and test loop.
