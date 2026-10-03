# ChronoLog tests

Tests are registered with CTest in every preset that builds them (`dev`, `asan`, `tsan`; `python` adds the binding
and MCP suites). Run them from the repository root:

```sh
ctest --preset dev                 # everything that the dev build registers
ctest --preset dev -R Contract     # one family by name
ctest --preset dev -N              # list without running
```

## Where tests live

| Location | What it holds |
|---|---|
| `tests/contract/` | The six contract suites (Clock, MetadataStore, Membership, Journal, TierStore, Replay) plus client, catalog, acquisition and archive suites. Each is value-parameterized; every implementation instantiates its suite in its own test directory (ARCHITECTURE.md section 15). `proto/` holds `ProtoContract.AdditiveCompatibility` and `NamingLint` against the checked-in baselines. The `*_guard_test.sh` scripts enforce module rules (no cross-service includes, no payload in logs, no SDK internals in public headers). |
| `src/<service>/tests/`, `client/*/tests`, `plugins/*/` | Component and adapter tests next to the code they cover. |
| `tests/integration/` | Real service processes on loopback ports: `dynamic_failover_integration` (dynamic membership, failover, destroy, abandonment), `visor.restart`, `visor.clock_skew`, `keeper.wal_restart`, `grapher.manifest_restart`, `grapher.compaction`, `lease.lifecycle`. |
| `deploy/local/tests/` | The `chronolog` launcher gates (`local.*`, `server.install`): readiness, restart, supervisor death, attach leases, tiers. |
| `tests/end-to-end/clock-skew/` | The clock-skew harness used by `visor.clock_skew`. |
| `tests/smoke/` | Container smoke. `run_dragon.sh` stages the native binaries into the runtime image, brings the compose stack up and runs the Python, MCP, TypeScript and plugin suites and the demo tour, under Docker then Podman (`ENGINES=podman` picks one). It serializes on a lock file under `~/chronolog-sprint/`. |

## Rules the suites follow

- Every test that starts ChronoLog service processes holds the ctest `RESOURCE_LOCK chronolog_stack`, so at most one
  runs at a time while unit tests keep running in parallel. A new real-process test must take it too.
- Gates assert outcomes, never latencies. Waits are bounded by a stated configured interval, never a bare sleep.
- A flaky test under load is a timing-dependent test: fix its design or the product race it exposes; never raise a
  timeout or add a retry to hide it.
- `gtest_discover_tests` always passes `DISCOVERY_TIMEOUT 60`.
- gRPC reports an oversize message as CANCELLED with an empty message, so delivery tests assert what arrived, not a
  status.
