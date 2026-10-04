# ChronoLog tests

Tests are registered with CTest in every preset that builds them (`dev`, `asan`, `tsan`; `python` adds the binding
and MCP suites). Run them from the repository root:

```sh
ctest --preset dev                 # everything that the dev build registers
ctest --preset dev -R '^keeper\.'  # one component
ctest --preset dev -N              # list without running
```

Every ctest is named `<component>.<name>`: `common.`, `keeper.`, `visor.`, `grapher.`, `player.`, `contract.`, `build.`,
`sdk.`, `context.`, `python.`, `typescript.`, `launcher.`, `integration.`, one prefix per plugin (`kvs.`, `pubsub.`,
`sql.`, `stream.`, `viz.`, `ldms.`, `mcp.`) and `bench.`. A gtest case registers as `<component>.<Suite>.<Case>`.

## Where tests live

| Location | What it holds |
|---|---|
| `tests/contract/` | The six contract suites (Clock, MetadataStore, Membership, Journal, TierStore, Replay). Each is value-parameterized; every implementation instantiates its suite in its own test directory (ARCHITECTURE.md section 15). `proto/` holds `contract.ProtoContract.AdditiveCompatibility` and `NamingLint` against the baselines in `proto/baseline/`. The `*_guard_test.sh` scripts enforce module rules (no cross-service includes, no payload in logs, no SDK internals in public headers). |
| `src/<component>/tests/`, `client/*/tests`, `plugins/*/` | Component and adapter tests next to the code they cover, including the wire-level adapter suites (`JournalAdapterTest`, `CatalogAdapterTest`, `ArchiveTransferTest`, `AcquisitionWatcherTest`, `ClientContract`). |
| `tests/integration/` | Real service processes on loopback ports, each script next to its driver: `dynamic/` (`visor.dynamic_failover`: dynamic membership, failover, destroy, abandonment), `visor/` (`visor.restart`, `visor.clock_skew`), `keeper_wal/` (`keeper.wal_restart`, `keeper.policy_marker`, `grapher.manifest_restart`, `grapher.compaction`), `lease/` (`visor.lease_lifecycle`) and `integration.bind_guard`. |
| `launcher/tests/` | The `chronolog` launcher gates (`launcher.*`): readiness, restart, supervisor death, attach leases, tiers, install. |
| `tests/smoke/` | Container smoke. `run.sh` stages the native binaries into the runtime image, brings the compose stack up and runs the Python, MCP, TypeScript and plugin suites and the demo tour, under Docker then Podman (`ENGINES=podman` picks one). It serializes on the lock file `CHRONOLOG_STACK_LOCK` names (default `~/chronolog-sprint/stack.lock`). |

## Rules the suites follow

- Every test that starts ChronoLog service processes holds the ctest `RESOURCE_LOCK chronolog_stack`, so at most one
  runs at a time while unit tests keep running in parallel. A new real-process test must take it too.
- Gates assert outcomes, never latencies. Waits are bounded by a stated configured interval, never a bare sleep.
- A flaky test under load is a timing-dependent test: fix its design or the product race it exposes; never raise a
  timeout or add a retry to hide it.
- `gtest_discover_tests` always passes `DISCOVERY_TIMEOUT 60`.
- gRPC reports an oversize message as CANCELLED with an empty message, so delivery tests assert what arrived, not a
  status.
