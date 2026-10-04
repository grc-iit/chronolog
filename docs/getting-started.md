# Getting started

This guide builds ChronoLog from source, runs a single node with the `chronolog` launcher, uses it from Python, C++
and TypeScript, connects an agent, and brings up the container stack. ChronoLog 4.0 is not published to PyPI or npm
yet, so every package below is built from the checkout.

## Requirements

- Linux on x86_64 with a C++20 compiler. The tree is built and tested with GCC 13.3.
- CMake 3.25 or newer (`CMakePresets.json` requires it) and Ninja.
- vcpkg in manifest mode. `vcpkg.json` pins gRPC, protobuf, GoogleTest, SQLite, nlohmann-json, HDF5, NuRaft and
  curl through its `builtin-baseline`; set `VCPKG_ROOT` to your vcpkg checkout.
- Python 3.12 or newer for the Python binding, the launcher and the MCP server.
- Node.js 22 or newer for the TypeScript binding.
- Rootless Podman or Docker with compose for the container stack.

## Build

```sh
git clone https://github.com/grc-iit/ChronoLog.git && cd ChronoLog
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The first configure builds the vcpkg dependencies, which takes a while; later configures reuse the binary cache.
Each preset builds into `build/<preset>`:

| Preset | Build |
| --- | --- |
| `dev` | Debug, warnings as errors |
| `release` | Release |
| `asan` | Debug with AddressSanitizer and UBSan (overlay triplet `x64-linux-asan`) |
| `tsan` | Debug with ThreadSanitizer (overlay triplet `x64-linux-tsan`) |
| `python` | `dev` plus the Python binding and the MCP plugin tests, in its own cache |
| `bench` | RelWithDebInfo with frame pointers and Google Benchmark for `tools/bench` |

The server binaries are `chrono_visor`, `chrono_keeper`, `chrono_grapher` and `chrono_player`.

## Install a single node

The launcher runs the Visor, a Keeper, the Grapher and the Player as local processes under one supervisor, without
containers. Install the stripped servers, the Python SDK, the launcher and the MCP server into a prefix you own:

```sh
prefix=$HOME/chronolog
cmake --preset release
cmake --build --preset release --target chrono_visor chrono_keeper chrono_grapher chrono_player
cmake --install build/release --prefix "$prefix" --component server --strip
python3 -m venv build/tools
build/tools/bin/python -m pip install build 'scikit-build-core>=1.1' 'nanobind>=3.1' hatchling
mkdir -p "$prefix/wheels"
build/tools/bin/python -m build --wheel --no-isolation -Ccmake.build-type=Release --outdir "$prefix/wheels" client/python
python3 launcher/build_wheel.py "$prefix/wheels"
build/tools/bin/python -m build --wheel --no-isolation --outdir "$prefix/wheels" plugins/mcp
python3 -m venv "$prefix"
"$prefix/bin/python" -m pip install --find-links "$prefix/wheels" "$prefix"/wheels/chronolog-*.whl \
  "$prefix"/wheels/chronolog_local-*.whl 'chronolog-mcp[local]==4.0.0'
export PATH="$prefix/bin:$PATH"
```

## Run it

```sh
chronolog doctor        # finds the four server executables
chronolog up            # creates and starts the instance "default"; prints its record as JSON
chronolog ls            # managed and registered instances
chronolog status        # per-service state of "default"
chronolog logs --service keeper --lines 20
chronolog down          # ordered stop; data is kept
```

`up` picks a free block of loopback ports and prints the endpoints. `chronolog env` prints them as shell exports
(`CHRONOLOG_CATALOG`, `CHRONOLOG_PLAYER`, `CHRONOLOG_HOME`, `CHRONOLOG_INSTANCE`), and `chronolog run -- <command>`
runs a command with those variables set while holding an attach lease on the instance. `down` refuses while another
process holds a live lease; `down --purge` also deletes the instance's data.

State lives under `$CHRONOLOG_HOME`, which defaults to `${XDG_STATE_HOME:-$HOME/.local/state}/chronolog`. Without
placement flags the WAL is under `$CHRONOLOG_HOME/instances/default/keeper/wal` and the archive under
`$CHRONOLOG_HOME/instances/default/grapher/archive`. To put them on chosen disks, pass the paths when the instance is
first created; later `up` calls reuse the saved paths and endpoints:

```sh
chronolog up --wal-dir /fast/nvme/chronolog-wal --local-root /nvme/chronolog-archive
```

Keep the WAL and the catalog for a restart; the archive alone is not a backup. If the supervisor is killed its
children die with it, and the next `chronolog up` restarts the instance with the same event ids and order. A Keeper
that dies is restarted by the supervisor with bounded backoff.

Add a slower tier, for example an NFS mount, and list the tiers. The Grapher migrates old archive files to it:

```sh
chronolog tier add default nfs /mnt/nfs/chronolog-tiers --rank 1 --kind slow
chronolog tier ls
```

## Python

Each example opens a story, appends one event DURABLE, reads until the Completion says the range is complete, then
tails exclusively after its first event. They read the endpoints from the environment that `chronolog run` sets and
can be rerun.

`quickstart.py`:

```python
import os
import time
import chronolog as cl

client = cl.connect(os.environ["CHRONOLOG_CATALOG"], os.environ["CHRONOLOG_PLAYER"])
try:
    client.create_chronicle("quickstart")
except cl.AlreadyExists:
    pass
try:
    story = client.create_story("quickstart", "python")
except cl.AlreadyExists:
    story = next(s for s in client.list_stories("quickstart") if s.name == "python" and not s.tombstoned)

with client.acquire(story, "quickstart-python") as writer:
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

```sh
chronolog run -- python quickstart.py
```

```text
appended EventId(story_id=2, writer_id=2, incarnation=1, sequence=1) acked: True
read 1 events, complete: True
tailed tailed from python True
```

A complete Read is final: no event in that range will ever appear later (ARCHITECTURE.md I6.8). When `complete` is
false, `reader.completion.reason` says why (`LAGGING_WRITERS`, `SOURCE_FAILED`, `TRUNCATED` or
`PHYSICAL_AXIS_UNBOUNDED`). More examples are in `client/python/tests/test_stack.py` and, for the Context API,
`client/python/tests/test_context.py`.

## C++

Install the client SDK from the release build tree and build against it with `find_package(chronolog)`:

```sh
cmake --build --preset release --target chronolog_client chronolog_client_static chronolog_context
cmake --install build/release --prefix build/sdk --component client
cmake -S quickstart -B build/quickstart -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux -DVCPKG_INSTALLED_DIR=$PWD/build/release/vcpkg_installed -DCMAKE_PREFIX_PATH=$PWD/build/sdk
cmake --build build/quickstart
chronolog run -- build/quickstart/quickstart
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
#include <cstdlib>
#include <iostream>
#include <thread>

namespace cl = chronolog::client;

int main()
{
    cl::ClientOptions options;
    options.catalog_endpoint = std::getenv("CHRONOLOG_CATALOG");
    options.player_endpoint = std::getenv("CHRONOLOG_PLAYER");
    auto client = cl::Client::Connect(options);
    if(!client.ok())
        return std::cerr << client.status() << "\n", 1;
    (void)client->createChronicle("quickstart"); // AlreadyExists on a rerun is fine
    auto story = client->createStory("quickstart", "cpp");
    if(!story.ok())
    {
        auto stories = client->listStories("quickstart");
        for(const auto& s: stories.value())
            if(s.name == "cpp" && !s.tombstoned)
                story = s;
    }
    auto writer = client->acquire(story->id, "quickstart-cpp");
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

The SDK's own acceptance test, `client/cpp/tests/acceptance.cpp`, covers retries, route adoption and leases.

## TypeScript

`bash tests/smoke/build_artifacts.sh` builds the Python and MCP wheels into `build/smoke/wheels/` and the package
`@chronolog/client` into `build/typescript/package/`:

```sh
mkdir app && cd app && npm init -y && npm install ../build/typescript/package
chronolog run -- node quickstart.mjs
```

`quickstart.mjs`:

```js
import chronolog from '@chronolog/client';
const { connect, AlreadyExists } = chronolog;

const client = await connect({ catalog: process.env.CHRONOLOG_CATALOG, player: process.env.CHRONOLOG_PLAYER });
try { await client.createChronicle('quickstart'); } catch (e) { if (!(e instanceof AlreadyExists)) throw e; }
let story;
try { story = await client.createStory('quickstart', 'typescript'); } catch (e) {
  if (!(e instanceof AlreadyExists)) throw e;
  story = (await client.listStories('quickstart')).find(s => s.name === 'typescript' && !s.tombstoned);
}

const writer = await client.acquire(story.id, 'quickstart-typescript');
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

More examples are in `client/typescript/test/client.test.js` and `client/typescript/test/context.test.js`.

## Agents: Claude Code, Codex and Clio-coder

Agents use ChronoLog through the Context API, exposed over MCP by `chronolog-mcp` (`plugins/mcp`). A context is one
story; an agent identity is a stable writer slot; `remember` stores a memory under a caller-chosen operation id, so a
retry never stores it twice; `recall` and `latest` read certified pages; `follow` waits on several contexts;
`reconcile` reports each uncertain write as LANDED, ABSENT or UNKNOWN after a crash. The twelve tools are listed in
[plugins/mcp/README.md](../plugins/mcp/README.md).

The repository root is a plugin marketplace. Its plugin `chronolog` installs the `chronolog` skill
(`skills/chronolog`) and the MCP server. Install it from a checkout, with the prefix from the install step on `PATH`
in the shell that starts the agent:

```sh
claude plugin marketplace add /path/to/ChronoLog && claude plugin install chronolog@chronolog --scope user
codex plugin marketplace add /path/to/ChronoLog && codex plugin add chronolog@chronolog
```

The plugin starts `chronolog run --up default -- chronolog-mcp`, which starts or attaches to the `default` instance
and holds a lease for the MCP process. Set `CHRONOLOG_MCP_IDENTITY` (a stable agent identity such as `team/planner`)
and `CHRONOLOG_CHRONICLE` for sessions that write. Closing a session drops its lease and leaves the services running.
A session that exits without `context_close` leaves its identity slot open: the next session with that identity
reads normally, and its `context_open` verdict asks it to call `context_reconcile` before writing.

Clio-coder declares the same server in its user `mcp.yaml`, which lives in the config directory that
`clio-coder paths` prints (`~/.config/clio-coder/mcp.yaml` by default):

```yaml
version: 1
servers:
  - id: chronolog
    command: /absolute/prefix/bin/chronolog
    args: [run, --up, default, --, /absolute/prefix/bin/chronolog-mcp]
    env:
      CHRONOLOG_MCP_IDENTITY: clio/main
      CHRONOLOG_CHRONICLE: agent-memory
    actionClass: execute
```

Clio-coder passes only `PATH`, `HOME`, the `XDG_*_HOME` variables and the locale to an MCP server, so this server uses
the same `default` instance as `chronolog up` and the Claude Code and Codex plugin. A `CHRONOLOG_HOME` exported in
the shell does not reach it; if you use one, add it under `env`. `clio-coder mcp list` shows the parsed declaration,
and a headless turn reaches the tools through Clio's gateway:

```sh
clio-coder run --autonomy yolo --timeout 600 "list the chronolog server's tools, then remember 'hello' in context demo"
```

Check both `verdict` and `answer_complete` in a tool result before trusting an answer.

## Containers

`deploy/demo/chronolog-demo` builds the runtime image from native binaries and runs the four services as separate
containers with compose (`deploy/compose/compose.yaml`). `--engine podman` or `--engine docker` selects the engine;
Podman is used when present.

```sh
deploy/demo/chronolog-demo build --engine podman   # binaries, wheels, TypeScript binding, images
deploy/demo/chronolog-demo up --engine podman
deploy/demo/chronolog-demo status --engine podman
deploy/demo/chronolog-demo tour --engine podman    # narrated, self-checking tour
deploy/demo/chronolog-demo down --engine podman
```

The Catalog listens on `127.0.0.1:50051` and Replay on `127.0.0.1:50054`; point the examples above at them with
`export CHRONOLOG_CATALOG=127.0.0.1:50051 CHRONOLOG_PLAYER=127.0.0.1:50054`. `up --full` adds Grafana, InfluxDB and
the stream and viz plugins (`deploy/compose/*.override.yaml`, dashboards in `deploy/grafana/`).

For more than one host, `tools/lab-cluster` is the harness the project uses for failover scenarios: three Visors in
dynamic membership mode, a Keeper and a Player per host and two Graphers on a shared archive. Its
`topology.py` names the lab's hosts; edit it for yours.
