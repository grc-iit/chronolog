---
sidebar_position: 5
title: "Testing Guidelines"
---

# Testing Guidelines

Every new feature and bug fix should include tests. ChronoLog uses **Google Test (GTest)** for unit tests and custom executables or shell scripts for the rest, registered through **CTest** except for the integration scripts that deploy their own daemons.

## Test Categories

| Category | Directory | Purpose | Needs a running deployment |
|---|---|---|---|
| Unit | `tests/unit/` | One component in isolation; also shell tests of the deploy and CI scripts | No |
| Functional | `tests/functional/` | One component's real code paths, such as file deletion | No |
| Integration | `tests/integration/<scope>/` | Two or more components talking to each other | Yes |
| Integration scripts | `tests/integration/*.sh` | Scenarios across keepers, graphers and players; each deploys and stops its own daemons | No (they start one) |
| End-to-end | `tests/end-to-end/` | Client APIs against a deployment: tail read, destroy, data integrity | Yes |
| Distributed | `tests/distributed/` | The NFS pass that the distributed pipeline runs | Run by the pipeline |
| Communication | `tests/communication/` | Thallium RPC and MPI | Yes |
| Overhead | `tests/overhead/` | Clock and lock costs | No |

## Writing Unit Tests

Unit tests use GTest and live under `tests/unit/<component>/`, with the component's name in the file name:

```
tests/unit/
├── chrono-common/
│   ├── chrono_common_story_chunk_test.cpp
│   └── chrono_common_archive_manifest_test.cpp
├── chrono-player/
│   └── chrono_player_archive_manifest_index_test.cpp
├── chrono-visor/
│   └── story_group_selection_test.cpp
└── ...
```

A unit test must not need running daemons. It may start a margo engine on loopback when the code under test is an RPC client or server, as `watermark_report_transport_test.cpp` does.

### CTest registration

Register GTest binaries with `gtest_discover_tests` and the prefix `Unit_<Component>_`, so every test case becomes its own CTest entry:

```cmake
add_executable(story_group_selection_test story_group_selection_test.cpp)
target_link_libraries(story_group_selection_test PRIVATE GTest::gtest_main chrono_common)
set_target_properties(story_group_selection_test PROPERTIES OUTPUT_NAME chronolog-test-story-group-selection)
gtest_discover_tests(story_group_selection_test TEST_PREFIX "Unit_ChronoVisor_")
```

A binary with its own `main()` is not a GTest binary: register it with a plain `add_test()`. `gtest_discover_tests` runs the binary at build time to list its tests, so a custom `main()` would run the whole program during the build.

### Best Practices

- Use **test fixtures** (`TEST_F`) to share setup and teardown between related tests.
- Keep each test on one behavior, and name it after that behavior: `TEST(StoryGroupSelection, SameStoryGetsTheSameGroupEveryTime)`.
- For a bug fix, write the test that reproduces the bug first and watch it fail.

## Writing Integration and End-to-End Tests

Integration and end-to-end tests are custom executables (GTest or not) that connect to a deployment someone else started. Name them `Integration_<Scope>_<Subject>` or `EndToEnd_<Area>_<Subject>`:

```cmake
add_test(NAME EndToEnd_TailRead_Playback
    COMMAND tail_read_playback_test --events 25 --max-wait 120)
set_tests_properties(EndToEnd_TailRead_Playback PROPERTIES TIMEOUT 180)
```

The local pipeline in CI runs them against its single-host deployment. Locally, start a deployment first; see [Running Tests](../development/running-tests.md#tests-that-need-a-running-deployment).

A scenario that needs control over the daemons themselves (stopping a grapher, freezing a keeper, patching the configuration) belongs in an integration script under `tests/integration/` that deploys its own daemons, like `watermark_loop_test.sh`.

## Test Expectations for PRs

| Change type | Expected tests |
|---|---|
| New public API or component | Unit tests covering the new interface |
| Bug fix | At least one test that reproduces the bug |
| Inter-component changes | Integration tests exercising the affected interaction |
| Keeper retention, watermark protocol, replay path or player | The four integration scripts, with their results in the PR |
| RPC endpoints, ports, configuration plumbing, deploy scripts | `docker/local_distributed_ci.sh` |
| Archive layout, manifest, archive reads or destroy | `docker/local_distributed_ci.sh --nfs` |
| All PRs | Existing tests must continue to pass |

## Disabled Tests

Use the `DISABLED_` prefix (GTest) or the CTest `DISABLED` property for a test that is temporarily broken while you land other work, and file an issue to track re-enabling it. `ctest -N` marks disabled entries, and `ctest -R <name>` runs one anyway.

## Further Reading

See [Running Tests](../development/running-tests.md) for the full test catalog and instructions on running tests locally, and [CI/CD](./ci-cd.md) for what the pipelines run.
