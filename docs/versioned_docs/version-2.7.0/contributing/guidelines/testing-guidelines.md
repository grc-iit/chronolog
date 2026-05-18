---
sidebar_position: 5
title: "Testing Guidelines"
---

# Testing Guidelines

Every new feature and bug fix should include tests. ChronoLog uses **Google Test (GTest)** for unit tests and custom executables for integration and system-level tests, all registered through **CTest**.

## Test Categories

| Category | Directory | Purpose |
|---|---|---|
| Unit | `test/unit/` | Verify individual components in isolation |
| Integration | `test/integration/` | Test interactions between two or more components |
| Communication | `test/communication/` | Validate RPC (Thallium) and MPI communication layers |
| Overhead | `test/overhead/` | Benchmark clock synchronization and lock contention |
| System | `test/system/` | End-to-end Python scripts for fidelity and workload simulation |

## Writing Unit Tests

Unit tests use the GTest framework and live under `test/unit/<component>/`.

### File Placement

Place test files alongside the component they test:

```
test/unit/
├── chrono_store/
│   ├── StoryChunkTest.cpp
│   └── StoryPipelineTest.cpp
├── chrono_player/
│   └── PlayerClientTest.cpp
└── ...
```

### CTest Naming

Register tests in CMake with the prefix `Unit_<Component>_<Subject>`:

```cmake
add_test(NAME Unit_ChronoStore_StoryChunk COMMAND StoryChunkTest)
```

### Best Practices

- Use **test fixtures** (`TEST_F`) to share setup/teardown logic across related tests.
- Keep each test focused on a single behavior.
- Name tests descriptively: `TEST_F(StoryChunkTest, RejectsEventsBeforeStartTime)`.

## Writing Integration Tests

Integration tests are custom executables (not necessarily GTest-based) under `test/integration/<scope>/`.

### CTest Registration

Use a descriptive prefix and mark tests that require a live deployment as `MANUAL`:

```cmake
add_test(NAME Integration_KeeperGrapher_StoryAcquisition COMMAND keeper_grapher_test)

# Tests that need running ChronoLog services:
add_test(NAME Integration_Client_RecordEvents COMMAND client_record_test)
set_tests_properties(Integration_Client_RecordEvents PROPERTIES LABELS "MANUAL")
```

Manual tests are excluded from the default `ctest` run and must be invoked explicitly:

```bash
ctest -L MANUAL
```

## Test Expectations for PRs

| Change type | Expected tests |
|---|---|
| New public API or component | Unit tests covering the new interface |
| Bug fix | At least one test that reproduces the bug |
| Inter-component changes | Integration tests exercising the affected interaction |
| All PRs | Existing tests must continue to pass |

## Disabled and Manual Tests

- **`DISABLED_` prefix** (GTest) — use when a test is temporarily broken and you need to land other work. File an issue to track re-enabling it.
- **`MANUAL` label** (CTest) — use for tests that require a running ChronoLog deployment. These run in CI during the deployment jobs but are skipped in local `ctest` runs.

## Further Reading

See [Running Tests](../development/running-tests.md) for the full test catalog and instructions on executing tests locally.
