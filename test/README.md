# ChronoLog Tests

All ChronoLog tests live under this `test/` directory (there is no other test tree; legacy `ChronoStore/test/` has been removed). Tests are built only when `CHRONOLOG_BUILD_TESTING` is ON (default) **and** `CMAKE_BUILD_TYPE` is `Debug`; they are then registered with CTest. Release builds do not include the test tree, so no test executables are built or installed in Release (the installation folder stays free of tests). To run tests, configure with `-DCMAKE_BUILD_TYPE=Debug`.

## Layout

| Directory | Description |
|-----------|-------------|
| **unit/** | Unit tests; **every test lives in a component subfolder**. `chrono-common/`, `chrono-store/`, `chrono-player/`, `chronokvs/`. All sources follow `component_element_test.cpp` (e.g. `chrono_common_story_chunk_test.cpp`, `chrono_store_hdf5_archiver_test.cpp`). |
| **integration/** | Integration tests: `client/`, `keeper-grapher/`, chronokvs, package-discovery. keeper-grapher `extract_test` and `ingest_test` are registered with CTest but are integration/manual (not installed to `chronolog/tests/`). Client tests are registered as MANUAL CTest and optionally installed when `CHRONOLOG_INSTALL_TESTS` is ON. |
| **communication/** | **Transport/protocol-layer tests (Thallium).** Server repeater RPC, client send/recv or RDMA, MPI client; exercises Thallium in isolation, not the full ChronoLog client library. Build-tree only; not installed. Kept separate from **integration/** because integration tests are application-level (e.g. client vs Keeper); communication tests are transport-only. |
| **overhead/** | Overhead and micro-benchmarks: clock, lock. **Built only on x86** (uses SSE2 `emmintrin.h` and RDTSC). Build-tree only; not installed. Optional `kmod/` (kernel module) is not part of CTest. |
| **system/** | Python-based fidelity and system checks (not built by CMake). **Run manually** (see Manual tests below). |
| **synthetic_workload/** | Synthetic workload and performance scripts. **Run manually** (see Manual tests below). |

Kafka deploy/validation scripts live under `tools/deploy/others/Kafka/` (e.g. `read-write-test.sh`, `end-to-end-lat-test.sh`) and are not part of the CTest tree.

## Running tests

Tests are only built when `CMAKE_BUILD_TYPE=Debug`; use a Debug build directory to run CTest.

- **From build dir:**  
  `ctest` or `ctest -R Unit_` for unit tests, `ctest -R Integration_` for integration (excluding MANUAL), `ctest -R Integration_Client_` for client integration (MANUAL), etc. Use `ctest -N` to list all tests including MANUAL and DISABLED.

- **What runs where:**  
  (1) **CTest** — unit, integration (keeper-grapher, chronokvs, package-discovery), communication, overhead (on x86). (2) **Client integration** — all Client executables are registered as **MANUAL** CTest (`Integration_Client_*`). **All `Integration_Client_*` tests require a running ChronoVisor/Keeper** and are excluded from default `ctest`; run them with `ctest -R Integration_Client_ -M Manual` when the server is available, or run the binaries by hand with the correct config (e.g. `--conf <path>`). (3) **system/** and **synthetic_workload/** — not in CTest; run their scripts manually (see Manual tests).

- **Install layout:**  
  Test executables are installed under `<prefix>/chronolog/tests/` **only when `CHRONOLOG_INSTALL_TESTS` is ON** (default OFF). When ON, all installable tests are installed: unit (chrono-common, chrono-store, chrono-player, chronokvs), package-discovery, chronokvs integration, and client integration (`chronolog-test-client-*`). Communication, overhead, and keeper-grapher tests are never installed (build-tree only). See Installed test executables below.

## Install policy (what goes to `chronolog/tests/`)

One flag controls all test installation: **`CHRONOLOG_INSTALL_TESTS`** (CMake option, default OFF). When ON, every test that is designated installable is installed; when OFF, nothing is installed to `chronolog/tests/`. The default can be overridden by the `CHRONOLOG_INSTALL_TESTS` environment variable (e.g. `export CHRONOLOG_INSTALL_TESTS=ON`).

`<prefix>/chronolog/tests/` is for **post-install verification**. Only tests that are self-contained, portable, useful after install, and stable are designated installable (and then only installed when the flag is ON). The following are **never** installed (build-tree only): keeper-grapher (config + Keeper/Grapher required), communication (Thallium transport-layer), overhead (x86 or benchmark-only), system/, synthetic_workload/.

When adding new tests that should be installable: wrap `chronolog_install_target(<target> DESTINATION tests)` in `if(CHRONOLOG_INSTALL_TESTS)` and set `OUTPUT_NAME` to `chronolog-test-<component>-<name>`. Tests that require a running stack or special env should remain build-tree only (no install call).

## Naming

### Test source files (standard)

All test **source files** follow one pattern so component and purpose are clear from the filename:

**`<component>_<element>_test.cpp`** (or `_test.sh` / `_test.py` for scripts)

- **component** — snake_case area: `chrono_common`, `chrono_store`, `chrono_player`, `chronokvs`, `client`, `keeper_grapher`, `package_discovery`, `thallium`, `clock`, `lock`.
- **element** — what is tested (e.g. `story_chunk`, `extraction_chain`, `connect_rpc`, `high_resolution`, `overhead`).
- **`_test`** — marks the file as a test; suffix `.cpp`, `.sh`, or `.py`.

Examples:

| File | Component | Element |
|------|-----------|---------|
| `chrono_common_story_chunk_test.cpp` | chrono_common | story_chunk |
| `chrono_store_hdf5_archiver_test.cpp` | chrono_store | hdf5_archiver |
| `client_connect_rpc_test.cpp` | client | connect_rpc |
| `keeper_grapher_extract_test.cpp` | keeper_grapher | extract |
| `package_discovery_cmake_test.cpp` | package_discovery | cmake |
| `thallium_server_test.cpp` | thallium | server |
| `clock_high_resolution_test.cpp` | clock | high_resolution |
| `lock_overhead_test.cpp` | lock | overhead |

Helper files (e.g. `common.h`, `story_chunk_test_utils.h`) keep descriptive names without the component prefix. Scripts: `clock_cset_shield_test.sh`, `thallium_protocol_test.sh`; system/synthetic_workload scripts keep their current names.

### Installed test executables

When **`CHRONOLOG_INSTALL_TESTS`** is ON, test binaries installed under `chronolog/tests/` follow a single pattern:

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

Client integration tests are also installed (when the flag is ON) with:

| Installed name | Description |
|----------------|-------------|
| `chronolog-test-client-connect-rpc` | Client connect RPC test |
| `chronolog-test-client-metadata-rpc` | Client metadata RPC test |
| `chronolog-test-client-multi-pthread` | Multi-pthread client test |
| `chronolog-test-client-thread-interdependency` | Thread interdependency test |
| `chronolog-test-client-multi-storytellers` | Multi-storytellers client |
| `chronolog-test-client-story-reader` | Story reader client |
| `chronolog-test-client-multi-argobots` | Multi Argobots client test |
| `chronolog-test-client-multi-openmp` | Multi OpenMP client test |
| `chronolog-test-client-hybrid-argobots` | Hybrid MPI+Argobots client test |

So when `CHRONOLOG_INSTALL_TESTS` is ON, `ls chronolog/tests/` shows a consistent, sortable set (e.g. all `chronolog-test-*`).

### CTest names

All CTest names follow the pattern **`<Type>_<Component>_<WhatIsTested>`**:

- **Type**: `Unit`, `Integration`, `Communication`, `PackageDiscovery`, `Overhead`
- **Component**: `ChronoCommon`, `ChronoStore`, `ChronoPlayer`, `KeeperGrapher`, `Thallium`, `ChronoKVS`, `Discovery`, `Lock`, `Clock`
- **WhatIsTested**: Short descriptor (e.g. `StoryChunkExtract`, `HDF5ArchiverWriteReadRoundtrip`)

Examples: `Unit_ChronoCommon_StoryChunk_TestConstructors.testEmptyConstructor`, `Integration_ChronoKVS_PluginIntegration`, `PackageDiscovery_CMakeFindPackage`. Filter with `ctest -R Unit_`, `ctest -R Integration_`, or `ctest -R PackageDiscovery`.

Some tests are **DISABLED** (Margo/service tests: extraction chain, chunk consumer, transfer agent, playback service) or **MANUAL** (Thallium client, all Client integration tests `Integration_Client_*`, `Integration_ChronoKVS_PluginIntegration`, `Overhead_Clock_Script`, `Communication_Thallium_ProtocolScript`); run them explicitly or with `ctest -N` to list.

### Why are some tests disabled?

Four tests are disabled by default because they start **Margo/Thallium** services or agents that either run indefinitely (infinite or long-lived loops), expect a remote peer (e.g. a receiver or Grapher), or are designed to be stopped with SIGTERM. When run under CTest with no server and no timeout, they would hang, segfault, or fail. They remain in the suite so you can run them manually when the right environment is available (e.g. start a receiver, then run the test with a timeout or SIGTERM). The disabled tests are: **Unit_ChronoCommon_ExtractionChain**, **Unit_ChronoCommon_ChunkConsumerService**, **Unit_ChronoPlayer_StoryChunkTransferAgent**, **Unit_ChronoPlayer_PlaybackServiceLifecycle**.

## Manual tests

These are not run by default CTest; run them by hand when the right environment is available:

- **system/** — Fidelity and system checks:  
  `python test/system/fidelity_test_01.py <path>`, `fidelity_test_02.py`, `fidelity_test_03.py`, or `fidelity_test_all.py` as documented in the scripts.
- **synthetic_workload/** — Use `INSTALL_DIR` (default `$HOME/chronolog-install`) so that `$INSTALL_DIR/chronolog` is the work tree (bin, lib, conf). Run `distributed_syslog.sh`, `perf_test.sh` with a running stack.
- **overhead/clock/clock_cset_shield_test.sh** — Script for clock tests with cset shield (requires root/cgroup; not cgroupv2). Run from the build dir: `test/overhead/clock/clock_cset_shield_test.sh <path_to_clock_test_binary>` (e.g. the built `high_res_clock_test`). Registered as MANUAL CTest `Overhead_Clock_Script`.
- **communication/thallium_protocol_test.sh** — Thallium multi-node test for HPC (SLURM/squeue, ssh). Run with `-j`, `-s`, `-c`, `-n` as needed. Registered as MANUAL CTest `Communication_Thallium_ProtocolScript`.
- **overhead/kmod/** — Optional kernel module (`kmod_rdtsc_rdtscp`); not part of CTest. Build and load manually if needed.
- **Integration_ChronoKVS_PluginIntegration** — Requires a running ChronoLog stack and runs a long data-propagation wait; use `ctest -R Integration_ChronoKVS_PluginIntegration -M Manual` when the stack is available. If run without a server, the test exits 0 (skipped).

## Notes

- **system/** and **synthetic_workload/** are not part of the CMake/CTest tree; run their scripts manually as above.
- keeper-grapher and HDF5ArchiveReadingAgent tests skip (exit 0) when no config path is provided; run manually with `--conf <path>` for full runs.
- Some tests (e.g. extraction chain, chunk consumer, Thallium server/client) run long or require SIGTERM to stop; use timeouts or run in isolation as needed.
