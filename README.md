<p align="center">
  <img src=".github/assets/chronolog_logo.svg" alt="ChronoLog logo" width="40%">
</p>

<h1 align="center">ChronoLog</h1>

<p align="center"><strong>A distributed, time-ordered, tiered log store</strong></p>

ChronoLog 4.0 stores append-only logs called stories. Agents and programs append events to stories; every event gets
a total order from a hybrid logical clock assigned where it is accepted, and an append is acknowledged only once it
is fsync'd in a write-ahead log. Events then move from Keeper memory and the WAL to an archive and on to slower
tiers, while readers replay any time range as one ordered, deduplicated stream. Every read ends with a Completion
that says whether the answer holds every event that will ever exist in that range, and if not, why. Agents use it as
shared memory and provenance: what each agent wrote, in what order, and whether an answer is complete.

ChronoLog 4.0 is not released yet and is not published to PyPI or npm. Build it from source.

## Four services

```text
          create, acquire                 heartbeats, routes
   SDK ───────────────────►  Visor  ◄──────────────────────────┐
    │                                                           │
    │ append (DURABLE ack)                                      │
    ▼                                                           │
  Keeper ── HLC, WAL, sealed chunks ──►  Grapher ── archive, ──►  slower tiers
    ▲                                       │      manifests
    │ recent events                         │ archive files
  Player ◄──────────────────────────────────┘
    │ Read and Tail with a Completion
    ▼
   SDK
```

The **Visor** hosts the Catalog (chronicles, stories, writers, ids) and cluster membership. A **Keeper** accepts
appends, assigns the HLC and keeps the WAL. The **Grapher** moves sealed chunks into the archive and down the tier
chain. The **Player** serves Read and Tail, merging Keeper data with the archive.
[docs/architecture.md](docs/architecture.md) explains how they fit together.

## Five-minute quick start

Requirements: Linux x86_64, GCC 13, CMake 3.25 or newer, Ninja, vcpkg (`VCPKG_ROOT`), Python 3.12 or newer.

Build the servers and install them with the Python SDK, the `chronolog` launcher and the MCP server into a prefix:

```sh
git clone https://github.com/grc-iit/ChronoLog.git && cd ChronoLog
export VCPKG_ROOT=/path/to/vcpkg
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

Start a single node (all four services as local processes) and check it:

```sh
chronolog up
chronolog status
```

Append and replay from Python. `chronolog run` passes the instance's endpoints in `CHRONOLOG_CATALOG` and
`CHRONOLOG_PLAYER`:

```python
# hello.py
import os
import time
import chronolog as cl

client = cl.connect(os.environ["CHRONOLOG_CATALOG"], os.environ["CHRONOLOG_PLAYER"])
try:
    client.create_chronicle("hello")
except cl.AlreadyExists:
    pass
story = client.create_story("hello", "run-" + str(os.getpid()))
with client.acquire(story, "hello-writer") as writer:
    result = writer.append(b"hello, world", content_type="text/plain")  # returns once fsync'd

# Replay up to just past the event; a Read is complete once every Keeper has sealed past its end.
end = cl.Hlc(result.hlc.physical_ns, result.hlc.logical + 1)
while True:
    with client.read(story, None, end) as reader:
        events = list(reader)
    if reader.completion.complete:
        break
    time.sleep(0.1)
for event in events:
    print(event.id.sequence, event.payload.decode())
print("complete:", reader.completion.complete)
```

```sh
chronolog run -- python hello.py
```

```text
1 hello, world
complete: True
```

A complete Read is final; an incomplete one names its reason, and you read again or continue from its frontier.

Connect an agent. The repository root is a plugin marketplace for Claude Code and Codex; its plugin installs the
`chronolog` skill and starts `chronolog-mcp` against the `default` instance:

```sh
claude plugin marketplace add "$PWD" && claude plugin install chronolog@chronolog --scope user
codex plugin marketplace add "$PWD" && codex plugin add chronolog@chronolog
```

Stop the node with `chronolog down`; its data is kept for the next `up`.
[docs/getting-started.md](docs/getting-started.md) covers the build presets, the launcher, C++ and TypeScript,
Clio-coder and the container stack.

## Repository map

| Directory | Contents |
| --- | --- |
| `include/chronolog/` | The six contracts (Clock, MetadataStore, Membership, Journal, TierStore, Replay) and shared types |
| `proto/` | The client API `chronolog.v1`, the internal API `chronolog.internal.v1` and their compatibility baselines |
| `src/` | The services: `visor/`, `keeper/`, `grapher/`, `player/`, and `common/` for code they share |
| `client/` | The C++ SDK and Context API (`cpp/`), the Python binding (`python/`), the TypeScript binding (`typescript/`) |
| `plugins/` | kvs, pubsub, sql, mcp, stream, viz, ldms ([plugins/README.md](plugins/README.md)) |
| `launcher/` | The `chronolog` CLI that runs a single node and manages instances and tiers |
| `skills/` | The `chronolog` agent skill shipped by the Claude Code and Codex plugin |
| `deploy/` | Containerfiles (`containers/`), compose files (`compose/`), the demo (`demo/`), Grafana dashboards (`grafana/`) |
| `tests/` | Contract suites, integration tests and the container smoke ([tests/README.md](tests/README.md)) |
| `tools/` | Benchmarks (`bench/`) and the multi-host lab harness (`lab-cluster/`) |
| `cmake/`, `triplets/` | CMake helpers and the vcpkg overlay triplets for the sanitizer presets |

## Documentation

- [docs/getting-started.md](docs/getting-started.md): build, run a node, use the SDKs, connect agents, containers.
- [docs/tour.md](docs/tour.md): a one-hour guided tour through three demos and the code behind each.
- [docs/architecture.md](docs/architecture.md): the services, the life of an event, Completion, tiers, failover.
- [CONTRIBUTING.md](CONTRIBUTING.md): building and testing, module rules, code style, commits.
- [ARCHITECTURE.md](ARCHITECTURE.md): the specification. Every rule has an id and a test that enforces it.

## License and acknowledgment

ChronoLog is distributed under the [BSD 2-Clause License](LICENSE).

<p align="center">
  <img src=".github/assets/grc-logo.png" alt="Gnosis Research Center" width="60">
</p>

<p align="center">
  <strong>Gnosis Research Center</strong>, Illinois Institute of Technology<br>
  <a href="https://grc.iit.edu">grc.iit.edu</a> · <a href="https://github.com/grc-iit/ChronoLog/issues">GitHub Issues</a>
</p>

<p align="center">
  <strong>Sponsored by:</strong><br>
  <a href="https://www.nsf.gov"><img src=".github/assets/nsf-logo.png" alt="National Science Foundation" width="100"></a><br>
  National Science Foundation (NSF CSSI-2104013)
</p>
