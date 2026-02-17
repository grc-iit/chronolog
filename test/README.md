# ChronoLog Tests

All ChronoLog tests live under this `test/` directory. They are built when `CHRONOLOG_BUILD_TESTING` is ON (default) and registered with CTest.

## Layout

| Directory | Description |
|-----------|-------------|
| **unit/** | Unit tests (GTest and standalone executables). Component-specific tests live in subdirs: `chrono_common/`, `ChronoStore/`, `ChronoPlayer/`. In-tree GTest tests: StoryChunk, StoryPipeline, ChronoKVSLogger. |
| **integration/** | Integration tests: Client, Keeper-Grapher, chronokvs, package-discovery. |
| **communication/** | Thallium protocol / RPC tests. |
| **overhead/** | Overhead and micro-benchmarks: clock, lock. **Built only on x86** (uses SSE2 `emmintrin.h` and RDTSC). |
| **system/** | Python-based fidelity and system checks (not built by CMake). Run manually, e.g. `python test/system/fidelity_test_01.py <path>`. |
| **synthetic_workload/** | Synthetic workload and performance scripts (run manually). |

## Running tests

- **From build dir:**  
  `ctest` or `ctest -R Unit_` for unit tests, `ctest -R Integration` for integration, etc.

- **Install layout:**  
  Test executables are installed under `chronolog/tests/` when using `chronolog_install_target(… DESTINATION tests)`.

## Naming

### Installed test executables

All test binaries installed under `chronolog/tests/` follow a single pattern:

**`chronolog-test-<component>-<name>`**

- **component** identifies the area: `common`, `store`, `player`, `chronokvs`, `discovery`.
- **name** is a short, hyphenated descriptor.

| Installed name | Description |
|----------------|------------|
| `chronolog-test-common-extraction-chain` | StoryChunk extraction chain (extractor chain, RDMA/CSV) |
| `chronolog-test-common-chunk-consumer-service` | Standalone StoryChunk consumer service |
| `chronolog-test-common-story-chunk` | StoryChunk GTest unit tests |
| `chronolog-test-common-story-pipeline` | StoryPipeline GTest unit tests |
| `chronolog-test-store-hdf5-archiver` | HDF5 archiver write/read/range tests |
| `chronolog-test-store-vlen-bytes-vs-blob-map` | HDF5 VLEN bytes vs blob+map comparison |
| `chronolog-test-player-transfer-agent` | StoryChunk transfer agent |
| `chronolog-test-player-playback-service` | Playback service |
| `chronolog-test-player-hdf5-archive-reader` | HDF5 archive reading agent |
| `chronolog-test-player-fs-monitoring` | File system monitoring |
| `chronolog-test-chronokvs-logger` | ChronoKVS logger GTest unit tests |
| `chronolog-test-chronokvs-integration` | ChronoKVS integration test |
| `chronolog-test-discovery-cmake` | find_package(Chronolog) discovery |
| `chronolog-test-discovery-pkgconfig` | pkg-config discovery |

So `ls chronolog/tests/` shows a consistent, sortable set (e.g. all `chronolog-test-*`).

### CTest names

- CTest names use a consistent prefix when possible: `Unit_<Component>_<Test>`, `PackageDiscovery_*`, etc., so you can filter with `ctest -R Unit_` or `ctest -R PackageDiscovery`.

## Notes

- **system/** and **synthetic_workload/** are not part of the CMake/CTest tree; run their scripts as needed.
- Some tests (e.g. extraction chain, chunk consumer, Thallium server/client) run long or require SIGTERM to stop; use timeouts or run in isolation as needed.
