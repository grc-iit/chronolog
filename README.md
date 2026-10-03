<p align="center">
  <img src="docs/static/logos/chronolog_logo.svg" alt="ChronoLog logo" width="40%">
</p>

<h1 align="center">ChronoLog</h1>

<p align="center"><strong>A distributed, time-ordered, tiered log store</strong></p>

ChronoLog 4.0 stores append-only logs called stories. Every event gets a total order from a hybrid logical clock
assigned where it is accepted, lands durably in a Keeper write-ahead log, and moves on to an archive tier, while
readers replay any time range from one ordered, deduplicated stream. Agents use it as shared memory and provenance:
what each agent wrote, in what order, and whether an answer covers everything that will ever exist in that range.
The design goal is correctness under failure. A read says when it is complete, and when it cannot be, it says why.

ChronoLog 4.0 is not released yet and is not published to PyPI or npm. Build it from source as described below.

## Architecture in one screen

[ARCHITECTURE.md](ARCHITECTURE.md) is the authority for everything in this section; the ids below point into it.

- **Four services** (section 2). The **Visor** hosts the Catalog (chronicles, stories, writer acquisition, ids) and
  cluster membership. A **Keeper** accepts appends, assigns the HLC and keeps the WAL. The **Grapher** moves sealed
  chunks into the HDF5 archive. The **Player** serves Replay, merging hot Keeper data with the archive.
- **Six contracts** (section 9). Clock, MetadataStore, Membership, Journal, TierStore and Replay are pure C++
  interfaces in `include/chronolog/`, each with a contract test suite every implementation instantiates (section 15).
- **Three time fields** (section 3). `physical` is the writer's wall clock, `hlc` is assigned by the Keeper on accept
  and defines total order and causality, and the EventId (story, writer, incarnation, sequence) is identity, the
  basis for idempotent retries (I5.4).
- **ACCEPTED and DURABLE** (section 5). ACCEPTED means in Keeper RAM and lost if the Keeper crashes; DURABLE means
  fsync'd in the WAL. DURABLE is the default and the only level called an ack (I5.1, I5.6).
- **Explicit Completion** (section 6). Every Read ends with a Completion. `complete=true` means every event any
  writer will ever produce in the range is in the stream (I6.8). Otherwise the reason says why: `LAGGING_WRITERS`
  names writers whose sealed frontier is below the end (I6.1), `SOURCE_FAILED` means a Keeper or archive source did
  not answer (I6.3), and `TRUNCATED` marks a bounded answer whose frontier is a complete prefix you continue from
  (I6.12). A Tail never claims completeness (I6.5).

Core has no TLS, authentication or replication of events and assumes a trusted network (section 14).

## Build from source

The tree is C++20 with CMake 3.25 or newer, Ninja and vcpkg in manifest mode (`vcpkg.json` pins gRPC, protobuf,
SQLite, HDF5, NuRaft, GoogleTest and their versions through its baseline). The Python binding needs Python 3.12 or
newer and the TypeScript binding Node.js 22 or newer. Containers run rootless under Podman or Docker. The commands
below were run on Ubuntu 24.04 with GCC 13.3, CMake 3.28, Python 3.12, Node.js 22 and Podman 4.9.

```sh
git clone https://github.com/grc-iit/ChronoLog.git && cd ChronoLog
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset dev
cmake --build --preset dev
```

Presets (`CMakePresets.json`): `dev` (Debug), `release`, `asan` (AddressSanitizer and UBSan), `tsan`
(ThreadSanitizer), `bench` (RelWithDebInfo with frame pointers) and `python` (dev plus the Python binding). Each
builds into `build/<preset>`.

## One node with agents

The `chronolog-local` launcher runs the Visor, Keeper, Grapher and Player as local processes without containers.
Python 3.12 or newer is required. The rehearsal uses the gated source at
`d74b850f7c098a6c7106024336288fd6a87481fe` on Dragon; install into a fresh user-owned prefix, not a system path.
From that checkout, with the build toolchain above available:

```sh
sha=d74b850f7c098a6c7106024336288fd6a87481fe
prefix="$HOME/chronolog-demo/$sha"
mkdir -p "$prefix/wheels"
cmake --preset release
cmake --build --preset release --target chrono_visor chrono_keeper chrono_grapher chrono_player
cmake --install build/release --prefix "$prefix" --component server --strip
python3 -m venv build/demo1-tools
build/demo1-tools/bin/python -m pip install build 'scikit-build-core>=1.1' 'nanobind>=3.1' hatchling
build/demo1-tools/bin/python -m build --wheel --no-isolation -Ccmake.build-type=Release --outdir "$prefix/wheels" client/python
python3 deploy/local/build_wheel.py "$prefix/wheels"
build/demo1-tools/bin/python -m build --wheel --no-isolation --outdir "$prefix/wheels" plugins/chrono-mcp
python3 -m venv "$prefix"
"$prefix/bin/python" -m pip install --find-links "$prefix/wheels" "$prefix"/wheels/chronolog-*.whl "$prefix"/wheels/chronolog_local-*.whl 'chronolog-mcp[local]==4.0.0'
export PATH="$prefix/bin:$PATH" CHRONOLOG_HOME="$prefix/state"
```

`doctor` resolves the four installed executables. `up` waits for readiness and prints the endpoints; `ls` reports
managed and registered external instances. This placement uses Dragon's `/home` NVMe for the WAL and its `/` NVMe
for the local archive (`/tmp` is on `/` there):

```sh
chronolog doctor
chronolog up --wal-dir "$prefix/wal" --local-root "/tmp/chronolog-demo-$(id -u)/$sha/archive"
chronolog ls
chronolog status
```

Choose a persistent directory on the desired filesystem for real use; `/tmp` is disposable rehearsal storage.
Without placement flags, the WAL is under `$CHRONOLOG_HOME/instances/default/keeper/wal` and the local tier under
`$CHRONOLOG_HOME/instances/default/grapher/archive`. The catalog, logs and registry also live under
`$CHRONOLOG_HOME`; its default is `$XDG_STATE_HOME/chronolog`, or `~/.local/state/chronolog`.
Placement flags choose paths when creating an instance. Subsequent `up` calls reuse the saved paths and endpoints.
Keep both the WAL and catalog state for restart; an archive alone is not an instance backup.

Install the Claude Code marketplace and plugin from the local checkout, with the prefix on PATH in the shell
that starts Claude Code:

```sh
claude plugin marketplace add "$prefix/checkout"
claude plugin install chronolog@chronolog --scope user
claude plugin list --json
```

The rehearsal stages the gated checkout at `$prefix/checkout`. In your own source install, use your checkout's
absolute path instead. These plugin installation commands do not log in to Claude Code; the PI must authenticate
before an interactive agent session if the existing login has expired.

The plugin selects `chronolog run --up default -- chronolog-mcp` when the launcher is on PATH. The wrapper supplies
endpoints and holds an instance lease for the MCP process. Set a stable `CHRONOLOG_MCP_IDENTITY` and
`CHRONOLOG_CHRONICLE` in the launching shell for writable sessions. Closing an MCP session drops its lease;
the default `on_last_detach=keep` policy leaves the services running. At creation, `--on-last-detach stop` sets
stop after the last lease and idle grace; `--ephemeral` selects that policy with a 30-second default grace.
`--idle-grace-s` chooses the grace. `down` stops services in order and preserves data, and refuses foreign live
leases. Close the session holding such a lease first.

```sh
chronolog down
chronolog ls
```

After a supervisor SIGKILL its children are killed too; `chronolog up` restarts from the saved instance.
The supervisor automatically restarts a dead Keeper with bounded backoff; inspect `status` and read Completion
before drawing conclusions during recovery. An immediate recall can be incomplete, or can already be complete
when the archive covers the requested range. The rehearsal prints the actual answer and checks that recovery
returns identical EventIds and HLCs.

Codex was not installed on the rehearsal host. Add the local checkout to its plugin marketplace and install
`chronolog@chronolog` through its plugin interface; alternatively configure an MCP server with command `chronolog` and arguments
`["run", "--up", "default", "--", "chronolog-mcp"]` in the installed Codex's MCP configuration.
For clio-coder, its Library can adopt skill resources from the locally installed Claude plugin; that import omits
MCP metadata. Declare the server separately in user `<config>/mcp.yaml` (or project `.clio-coder/mcp.yaml`):

```yaml
version: 1
servers:
  - id: chronolog
    command: /absolute/path/to/prefix/bin/chronolog
    args: [run, --up, default, --, /absolute/path/to/prefix/bin/chronolog-mcp]
    env:
      CHRONOLOG_HOME: /absolute/path/to/prefix/state
      CHRONOLOG_MCP_IDENTITY: clio/main
      CHRONOLOG_CHRONICLE: agent-memory
    actionClass: execute
```

`actionClass` is allowed only at user scope; project declarations require explicit operator trust. Configure
clio-coder on the machine hosting the instance; its stdio declaration does not itself connect to Dragon over SSH.

## Run a stack

`deploy/demo/chronolog-demo` builds the runtime image from the native binaries and brings up the Visor, a Keeper,
the Grapher and the Player with compose. `--engine podman` or `--engine docker` picks the engine (Podman when
present).

```sh
deploy/demo/chronolog-demo build --engine podman   # binaries, Python and MCP wheels, TypeScript binding, images
deploy/demo/chronolog-demo up --engine podman
deploy/demo/chronolog-demo status --engine podman
```

The Catalog listens on `127.0.0.1:50051` and Replay on `127.0.0.1:50054`. `chronolog-demo tour` runs a narrated,
self-checking tour of the stack, `up --full` adds Grafana, InfluxDB and the stream and viz plugins, and
`chronolog-demo down` stops it. The compose files are in `deploy/compose/`.

## Quickstarts

Each program opens a story, appends DURABLE, reads until the Completion is complete, then tails exclusively after
its first event. They run against the stack above and can be rerun.

### C++

Install the client SDK from the build tree and build against it with `find_package(chronolog)`:

```sh
cmake --install build/dev --prefix build/sdk --component client
cmake -S quickstart -B build/quickstart -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_INSTALLED_DIR=$PWD/build/dev/vcpkg_installed -DCMAKE_PREFIX_PATH=$PWD/build/sdk
cmake --build build/quickstart && build/quickstart/quickstart
```

`quickstart/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.25)
project(quickstart LANGUAGES CXX)
find_package(chronolog CONFIG REQUIRED)
add_executable(quickstart main.cpp)
target_link_libraries(quickstart PRIVATE chronolog::client)
```

`quickstart/main.cpp`:

```cpp
#include <chronolog/client/client.h>
#include <iostream>
#include <thread>

namespace cl = chronolog::client;

int main()
{
    cl::ClientOptions options;
    options.catalog_endpoint = "127.0.0.1:50051";
    options.player_endpoint = "127.0.0.1:50054";
    auto client = cl::Client::Connect(options);
    if(!client.ok())
        return std::cerr << client.status() << "\n", 1;
    (void)client->createChronicle("readme"); // AlreadyExists on a rerun is fine
    auto story = client->createStory("readme", "cpp");
    if(!story.ok())
    {
        auto stories = client->listStories("readme");
        for(const auto& s: stories.value())
            if(s.name == "cpp" && !s.tombstoned)
                story = s;
    }
    auto writer = client->acquire(story->id, "readme-cpp");
    if(!writer.ok())
        return std::cerr << writer.status() << "\n", 1;

    cl::AppendSpec spec; // durability defaults to Durable
    spec.envelope.content_type = "text/plain";
    spec.envelope.payload = "hello from c++";
    auto first = writer->append(spec);
    if(!first.ok())
        return std::cerr << first.status() << "\n", 1;
    std::cout << "appended sequence " << first->event_id.sequence << " acked: " << first->acked() << "\n";

    // Read up to just past our event; retry until the Completion says the range is complete.
    chronolog::Hlc end{first->hlc.physical_ns, first->hlc.logical + 1};
    for(;;)
    {
        auto reader = client->read(story->id, {{}, end});
        size_t events = 0;
        std::optional<chronolog::Completion> completion;
        for(;;)
        {
            auto item = reader->next();
            if(!item.ok() || !item->has_value())
                break;
            events += (*item)->events.size();
            if((*item)->completion)
                completion = (*item)->completion;
        }
        if(completion && completion->complete)
        {
            std::cout << "read " << events << " events, complete: 1\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Tail resumes exclusively after a Position, so it delivers only the next event.
    spec.envelope.payload = "tailed from c++";
    auto second = writer->append(spec);
    auto tail = client->tail(story->id, cl::Position{first->hlc, first->event_id});
    for(;;)
    {
        auto item = tail->next(std::chrono::system_clock::now() + std::chrono::seconds(10));
        if(!item.ok() || !item->has_value())
            return std::cerr << "tail ended\n", 1;
        if(!(*item)->events.empty())
        {
            const auto& event = (*item)->events.front();
            std::cout << "tailed " << event.envelope.payload << " " << (event.id == second->event_id) << "\n";
            break;
        }
    }
    tail->cancel();
    (void)writer->release();
}
```

```text
appended sequence 1 acked: 1
read 1 events, complete: 1
tailed tailed from c++ 1
```

### Python

`chronolog-demo build` leaves the wheel in `build/smoke/wheels/`:

```sh
python3 -m venv .venv && .venv/bin/pip install build/smoke/wheels/chronolog-4.0.0-*.whl
.venv/bin/python quickstart.py
```

```python
import time
import chronolog as cl

client = cl.connect("127.0.0.1:50051", "127.0.0.1:50054")
try:
    client.create_chronicle("readme")
except cl.AlreadyExists:
    pass
try:
    story = client.create_story("readme", "python")
except cl.AlreadyExists:
    story = next(s for s in client.list_stories("readme") if s.name == "python" and not s.tombstoned)

with client.acquire(story, "readme-python") as writer:
    first = writer.append(b"hello from python", content_type="text/plain")  # DURABLE by default
    print("appended", first.event_id, "acked:", first.acked)

    # Read up to just past our event; retry until the Completion says the range is complete.
    end = cl.Hlc(first.hlc.physical_ns, first.hlc.logical + 1)
    while True:
        with client.read(story, None, end) as reader:
            events = list(reader)
        if reader.completion.complete:
            break
        time.sleep(0.1)
    print("read", len(events), "events, complete:", reader.completion.complete)

    # Tail resumes exclusively after a Position, so it delivers only the next event.
    second = writer.append(b"tailed from python", content_type="text/plain")
    with client.tail(story, cl.Position(first.hlc, first.event_id), timeout=10) as tail:
        event = next(iter(tail))
    print("tailed", event.payload.decode(), event.id == second.event_id)
```

```text
appended EventId(story_id=2, writer_id=2, incarnation=1, sequence=1) acked: True
read 1 events, complete: True
tailed tailed from python True
```

### TypeScript

`chronolog-demo build` leaves the package `@chronolog/client` in `build/typescript/package/`:

```sh
mkdir app && cd app && npm init -y && npm install ../build/typescript/package
node quickstart.mjs
```

```js
import chronolog from '@chronolog/client';
const { connect, AlreadyExists } = chronolog;

const client = await connect({ catalog: '127.0.0.1:50051', player: '127.0.0.1:50054' });
try { await client.createChronicle('readme'); } catch (e) { if (!(e instanceof AlreadyExists)) throw e; }
let story;
try { story = await client.createStory('readme', 'typescript'); } catch (e) {
  if (!(e instanceof AlreadyExists)) throw e;
  story = (await client.listStories('readme')).find(s => s.name === 'typescript' && !s.tombstoned);
}

const writer = await client.acquire(story.id, 'readme-typescript');
const first = await writer.append(Buffer.from('hello from typescript'), { contentType: 'text/plain' }); // DURABLE by default
console.log('appended', first.eventId.sequence, 'acked:', first.acked);

// Read up to just past our event; retry until the Completion says the range is complete.
const end = { physicalNs: first.hlc.physicalNs, logical: first.hlc.logical + 1 };
for (;;) {
  const stream = client.read(story.id, { start: { physicalNs: 0n, logical: 0 }, end });
  const events = [];
  for await (const event of stream) events.push(event);
  const completion = await stream.completion;
  if (completion.complete) { console.log('read', events.length, 'events, complete:', completion.complete); break; }
  await new Promise(resolve => setTimeout(resolve, 100));
}

// Tail resumes exclusively after a Position, so it delivers only the next event.
const second = await writer.append(Buffer.from('tailed from typescript'), { contentType: 'text/plain' });
const tail = client.tail(story.id, { hlc: first.hlc, id: first.eventId });
for await (const event of tail) {
  console.log('tailed', Buffer.from(event.envelope.payload).toString(), event.id.sequence === second.eventId.sequence);
  break;
}
await writer.release();
```

```text
appended 1n acked: true
read 1 events, complete: true
tailed tailed from typescript true
```

## Agents

The Context API (`client/cpp/context`, bound in Python and TypeScript) is the agent layer over the SDK. A context is
one story; an agent identity is a stable writer slot; `remember` stores a memory under a caller-chosen operation id,
so a retry after an error or timeout never stores it twice; `recall` and `latest` read certified pages at a verified
cut; `follow` waits on several contexts at once; and `reconcile` recovers writes whose outcome is unknown after a
crash or fence, reporting each operation LANDED, ABSENT or UNKNOWN. Checkpoints carry the processed position and
ownership so a restarted agent resumes without duplicating or silently dropping memories.

`chrono-mcp` exposes that layer to any MCP client as twelve tools: `instance_list`, `instance_control`, `context_open`, `context_remember`,
`context_recall`, `context_latest`, `context_follow`, `context_reconcile`, `context_checkpoint`, `context_close`,
`context_list` and `context_status` ([plugins/chrono-mcp/README.md](plugins/chrono-mcp/README.md)).

The repository root is a plugin marketplace for Claude Code and Codex. Its one plugin, `chronolog`, installs the
`chronolog` skill (`skills/chronolog`) and the chrono-mcp server:

The local installation above uses the marketplace's current launch path:

```text
chronolog run --up default -- chronolog-mcp
```

When no launcher is installed, its fallback is `uvx chronolog-mcp==4.0.0` for an existing deployment. The source
wheels must be made available through `UV_FIND_LINKS` until publication. `instance_list` discovers instances and
`instance_control` explicitly attaches, creates, detaches or stops them. `context_recall` accepts `since` and
exclusive `until` as int64 nanoseconds or RFC 3339. These bounds map to acceptance HLC time, not writer physical
time; check both `verdict` and `answer_complete`. Use the second event's acceptance nanoseconds as `since` and
the fifth event's as `until` to select the middle three of five distinct acceptance timestamps.

## Plugins

- **chrono-kvs**: a versioned key-value store with put, get, get as of an HLC, and history.
- **chrono-pubsub**: topics with at-least-once delivery and resumable consumers whose positions live in kvs.
- **chrono-sql**: append-only typed tables with a small SELECT that reports its Completion.
- **chrono-mcp**: twelve tools for Context memory and explicit local instance control over MCP.
- **chrono-stream**: host metrics into ChronoLog and a resumable exporter to InfluxDB with Grafana dashboards.
- **chrono-viz**: a Grafana datasource for ChronoLog with completeness notices and live tail.
- **chrono-ldms**: an LDMS store plugin that bridges sampler data into ChronoLog through a non-blocking queue.

## Tests

```sh
ctest --preset dev
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

`ctest --preset dev` runs the six contract suites, the component and adapter suites, and the integration tests that
start real services on loopback ports. The `python` preset adds the Python binding and MCP plugin suites.
`tests/smoke/run_dragon.sh` is the full container smoke: it stages the native binaries into the runtime image, brings
the compose stack up and runs the Python, MCP, TypeScript and plugin suites and the demo tour against it, under
Docker and then Podman (`ENGINES=podman` picks one). It serializes on a host lock file under `~/chronolog-sprint/`.

## License and acknowledgment

ChronoLog is distributed under the [BSD 2-Clause License](LICENSE).

<p align="center">
  <img src="website/public/images/logos/grc-logo.png" alt="Gnosis Research Center" width="60">
</p>

<p align="center">
  <strong>Gnosis Research Center</strong>, Illinois Institute of Technology<br>
  <a href="https://grc.iit.edu">grc.iit.edu</a> · <a href="https://github.com/grc-iit/ChronoLog/issues">GitHub Issues</a>
</p>

<p align="center">
  <strong>Sponsored by:</strong><br>
  <a href="https://www.nsf.gov"><img src="docs/static/logos/nsf-fb7efe9286a9b499c5907d82af3e70fd.png" alt="National Science Foundation" width="100"></a><br>
  National Science Foundation (NSF CSSI-2104013)
</p>
