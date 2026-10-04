# ChronoLog tests

Tests are registered with CTest in every preset that builds them (`dev`, `asan`, `tsan`; `python` adds the binding
and MCP suites). Run them from the repository root:

```sh
ctest --preset dev                 # everything the dev build registers
ctest --preset dev -R Contract     # one family by name
ctest --preset dev -N              # list without running
```

Component tests carry their component as a ctest prefix (`keeper.`, `grapher.`, `visor.`, `player.`, `kvs.` and so
on), so `ctest --preset dev -R '^keeper\.'` runs one component.

## Where tests live

| Location | What it holds |
| --- | --- |
| `tests/contract/` | The six contract suites (Clock, MetadataStore, Membership, Journal, TierStore, Replay) plus the client, catalog, acquisition and replay suites. Each is value-parameterized; every implementation instantiates its suite in its own test directory (ARCHITECTURE.md section 15). `proto/` holds `ProtoContract.AdditiveCompatibility` and the naming lint against the baselines in `proto/baseline/`. The `*_guard_test.sh` scripts enforce module rules: no generated types in contracts, no payload in logs, no SDK internals in public headers. |
| `src/<component>/tests/`, `client/*/tests`, `client/cpp/context/`, `plugins/<name>/tests` | Component tests and contract instantiations next to the code they cover, including each service's adapter tests. |
| `tests/integration/` | Real service processes on loopback ports: dynamic membership and failover (`dynamic/`), Visor restart and clock skew (`visor/`), Keeper WAL restart (`keeper_wal/`), writer lease lifecycle (`lease/`), and the scripts for manifest restart, compaction, the policy marker and the bind guard. |
| `launcher/tests/` | The `chronolog` launcher gates: readiness, restart, supervisor death, attach leases, tiers, the installed-server test. |
| `tools/lab-cluster/tests/` | Checks for the multi-host lab harness. |
| `tests/smoke/` | The container smoke. `build_artifacts.sh` builds the wheels and the TypeScript package; `run.sh` stages the native binaries into the runtime image, brings the compose stack up and runs the Python, MCP, TypeScript and plugin suites and the demo tour, under Docker then Podman (`ENGINES=podman` picks one). |

Benchmarks live in `tools/bench` and build only with the `bench` preset. They are not tests.

## Rules the suites follow

- Every test that starts ChronoLog service processes holds the ctest `RESOURCE_LOCK chronolog_stack`, so at most one
  runs at a time while unit tests keep running in parallel. A new real-process test must take it too.
- Stack tests pick loopback ports at random, wait a bounded time for readiness and retry on fresh ports when a bind
  fails (`client/cpp/tests/acceptance.sh` is the pattern to copy).
- A ctest whose command is a script that starts ChronoLog binaries begins with `${CMAKE_CROSSCOMPILING_EMULATOR}`, or
  ThreadSanitizer aborts it.
- Gates assert outcomes, never latencies. Waits are bounded by a stated configured interval, never a bare sleep.
- A flaky test under load is a timing-dependent test: fix its design or the product race it exposes; never raise a
  timeout or add a retry to hide it.
- `gtest_discover_tests` always passes `DISCOVERY_TIMEOUT 60`.
- A contract suite skips a case only where the instantiation cannot provide what it needs (a RAM Journal has no WAL);
  a gate that skips in every instantiation counts as missing.
- gRPC reports an oversize message as CANCELLED with an empty message, so delivery tests assert what arrived, not a
  status.
