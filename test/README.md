# ChronoLog Tests

All ChronoLog tests live under this `test/` directory (there is no other test tree; legacy `ChronoStore/test/` has been removed). Tests are built when `CHRONOLOG_BUILD_TESTING` is ON (default) and registered with CTest.

## Layout

| Directory | Description |
|-----------|-------------|
| **unit/** | Unit tests (GTest and standalone executables). Component-specific tests live in subdirs: `chrono_common/`, `ChronoStore/`, `ChronoPlayer/`. In-tree GTest tests: StoryChunk, StoryPipeline, ChronoKVSLogger. |
| **integration/** | Integration tests: Client, Keeper-Grapher, chronokvs, package-discovery. Keeper-Grapher `extract_test` and `ingest_test` are registered with CTest but are integration/manual (not installed to `chronolog/tests/`). |
| **communication/** | Thallium protocol / RPC tests (build-tree only; not installed). |
| **overhead/** | Overhead and micro-benchmarks: clock, lock. **Built only on x86** (uses SSE2 `emmintrin.h` and RDTSC). Build-tree only; not installed. |
| **system/** | Python-based fidelity and system checks (not built by CMake). **Run manually**, e.g. `python test/system/fidelity_test_01.py <path>`. |
| **synthetic_workload/** | Synthetic workload and performance scripts. **Run manually**; use `INSTALL_DIR` (default `$HOME/chronolog-install`) so that `$INSTALL_DIR/chronolog` is the work tree (bin, lib, conf). |

Kafka deploy/validation scripts live under `tools/deploy/others/Kafka/` (e.g. `read-write-test.sh`, `end-to-end-lat-test.sh`) and are not part of the CTest tree.

## Running tests

- **From build dir:**  
  `ctest` or `ctest -R Unit_` for unit tests, `ctest -R Integration` for integration, etc.

- **Install layout:**  
  A curated set of test executables is installed under `<prefix>/chronolog/tests/` (unit, package-discovery, chronokvs integration). Communication, overhead, Keeper-Grapher, and Client integration tests are build-tree only unless explicitly enabled (e.g. `CHRONOLOG_INSTALL_TESTS` for Client).

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

All CTest names follow the pattern **`<Type>_<Component>_<WhatIsTested>`**:

- **Type**: `Unit`, `Integration`, `Communication`, `PackageDiscovery`, `Overhead`
- **Component**: `ChronoCommon`, `ChronoStore`, `ChronoPlayer`, `KeeperGrapher`, `Thallium`, `ChronoKVS`, `Discovery`, `Lock`, `Clock`
- **WhatIsTested**: Short descriptor (e.g. `StoryChunkExtract`, `HDF5ArchiverWriteReadRoundtrip`)

Examples: `Unit_ChronoCommon_StoryChunk_TestConstructors.testEmptyConstructor`, `Integration_ChronoKVS_PluginIntegration`, `PackageDiscovery_CMakeFindPackage`. Filter with `ctest -R Unit_`, `ctest -R Integration_`, or `ctest -R PackageDiscovery`.

Some tests are **DISABLED** (Margo/service tests: extraction chain, chunk consumer, transfer agent, playback service) or **MANUAL** (Thallium client); run them explicitly or with `ctest -N` to list.

### Why are some tests disabled?

Four tests are disabled by default because they start **Margo/Thallium** services or agents that either run indefinitely (infinite or long-lived loops), expect a remote peer (e.g. a receiver or Grapher), or are designed to be stopped with SIGTERM. When run under CTest with no server and no timeout, they would hang, segfault, or fail. They remain in the suite so you can run them manually when the right environment is available (e.g. start a receiver, then run the test with a timeout or SIGTERM). The disabled tests are: **Unit_ChronoCommon_ExtractionChain**, **Unit_ChronoCommon_ChunkConsumerService**, **Unit_ChronoPlayer_StoryChunkTransferAgent**, **Unit_ChronoPlayer_PlaybackServiceLifecycle**.

## Notes

- **system/** and **synthetic_workload/** are not part of the CMake/CTest tree; run their scripts manually as needed.
- Keeper-Grapher and HDF5ArchiveReadingAgent tests skip (exit 0) when no config path is provided; run manually with `--conf <path>` for full runs.
- Some tests (e.g. extraction chain, chunk consumer, Thallium server/client) run long or require SIGTERM to stop; use timeouts or run in isolation as needed.
