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

`chrono-mcp` exposes that layer to any MCP client as ten tools: `context_open`, `context_remember`,
`context_recall`, `context_latest`, `context_follow`, `context_reconcile`, `context_checkpoint`, `context_close`,
`context_list` and `context_status` ([plugins/chrono-mcp/README.md](plugins/chrono-mcp/README.md)).

The repository root is a plugin marketplace for Claude Code and Codex. Its one plugin, `chronolog`, installs the
`chronolog` skill (`skills/chronolog`) and the chrono-mcp server:

```sh
claude plugin marketplace add /path/to/ChronoLog && claude plugin install chronolog@chronolog
codex plugin marketplace add /path/to/ChronoLog && codex plugin add chronolog@chronolog
```

The plugin launches the server as `uvx chronolog-mcp==4.0.0`. Until that package is published, point uv at the
wheels `chronolog-demo build` produced and the same command resolves them locally; set the stack endpoint and a
stable identity for writable tools the same way:

```sh
export UV_FIND_LINKS=$PWD/build/smoke/wheels
export CHRONOLOG_CATALOG=127.0.0.1:50051 CHRONOLOG_PLAYER=127.0.0.1:50054 CHRONOLOG_MCP_IDENTITY=me/agent
```

## Plugins

- **chrono-kvs**: a versioned key-value store with put, get, get as of an HLC, and history.
- **chrono-pubsub**: topics with at-least-once delivery and resumable consumers whose positions live in kvs.
- **chrono-sql**: append-only typed tables with a small SELECT that reports its Completion.
- **chrono-mcp**: the ten Context tools for agents over MCP.
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
