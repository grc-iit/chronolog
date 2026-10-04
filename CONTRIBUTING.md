# Contributing to ChronoLog

ChronoLog is a log store whose value is correctness under failure. A change is ready when its tests prove the
behavior it claims, the full test suite passes, and the rules below hold. [ARCHITECTURE.md](ARCHITECTURE.md) is the
specification; read [docs/architecture.md](docs/architecture.md) first for the map into it.

## Build and test locally

Set up the toolchain as in [docs/getting-started.md](docs/getting-started.md), then:

```sh
cmake --preset dev && cmake --build --preset dev
ctest --preset dev                      # everything the dev build registers
ctest --preset dev -R JournalContract   # one family by name
ctest --preset dev -N                   # list without running
```

Run the sanitizer presets for any change that touches threads, locks, memory ownership, includes or linking. A TSAN
report is a failure; suppressions need an RFC (ARCHITECTURE.md section 15).

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

The `python` preset builds the Python binding and adds its suites and the MCP plugin suites:

```sh
cmake --preset python && cmake --build build/python && ctest --test-dir build/python -R 'python|mcp'
```

The container smoke builds the wheels and the TypeScript package once, then brings up the compose stack and runs the
Python, MCP, TypeScript and plugin suites and the demo tour against it. Run it when your change affects code that
runs in the stack (services, bindings, plugins, Containerfiles, compose files):

```sh
bash tests/smoke/build_artifacts.sh
ENGINES=podman bash tests/smoke/run.sh
```

[tests/README.md](tests/README.md) describes where each kind of test lives.

## Writing tests

- Every implementation of a contract instantiates that contract's suite from `tests/contract/` in its own test
  directory. Add focused cases for logic the suite does not cover, next to the code.
- Tests assert outcomes, never latencies or throughput.
- A test that starts service processes holds the ctest `RESOURCE_LOCK chronolog_stack`, uses loopback ports picked at
  random with a retry on bind failure, and bounds every wait.
- Every `gtest_discover_tests` call passes `DISCOVERY_TIMEOUT 60`.
- Never weaken, skip or delete an assertion to get green. A flaky test is timing-dependent: fix its design or the race
  it exposes, never lengthen a timeout or add a sleep or retry to hide it.
- Test names that ARCHITECTURE.md cites are normative. Renaming one is a contract change.

## Module rules

ARCHITECTURE.md section 11 is the full table. In short:

- One change touches one owned directory plus its tests (M11.6). Each service under `src/`, each binding under
  `client/` and each plugin under `plugins/` is its own owned directory.
- No service includes another service. Shared code goes to `src/common/` (M11.2).
- `src/common/` does not include gRPC or protobuf, except `src/common/rpc/`, which holds the channel policy every
  service uses to reach a peer (M11.3). Every peer channel comes from there.
- Plugins and bindings use only the public SDK headers or bindings, never `src/` (section 11 table).
- Blocking calls (fsync, HDF5, NFS open) run on worker threads, never on a gRPC callback thread (M11.7).
- Contracts in `include/chronolog/` contain no gRPC or protobuf types (M11.1).

## Code style

- C++20. Format every changed C++ file with clang-format 18 and the style at the repository root:
  `clang-format -i <files>`. `.github/code-style/pre-commit` is a pre-commit hook that checks staged files.
- Logging uses `absl::log` (`LOG`, `VLOG`); `CHECK` only for programmer errors. No logging on per-event hot paths, and
  never log payload bytes (S14.5).
- The client SDK never writes to stderr.
- Keep the text of log lines that tests or scripts match.
- Comments are minimal and say why, not what.

## Commit messages

Plain imperative subjects that cite the invariant ids the change implements or tests, for example:

```text
Keeper rejects an append from a writer it is not assigned (I7.5)
```

Keep commits small. The body says what changed and why, and for a merge, which invariants its tests cover.

## Contract changes

Sections 3 through 10 and section 15 of ARCHITECTURE.md, the headers in `include/chronolog/`, the protos in `proto/`
and the contract suites in `tests/contract/` are frozen. Changing any of them is a contract change and needs, per
A16.3: an RFC describing the change, review by two reviewers, and the QA sign-off of the PI. The durable record is the
diff to ARCHITECTURE.md plus the merge commit message, which names both reviewers. Wire changes stay additive
(W10.1); a breaking change creates `chronolog.v2`.

Changes to sections 11 through 14 and 16 through 18 are editorial and need one reviewer (section 1).

## Conduct

See [.github/CODE_OF_CONDUCT.md](.github/CODE_OF_CONDUCT.md).
