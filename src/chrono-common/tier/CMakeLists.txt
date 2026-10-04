find_package(hdf5 CONFIG REQUIRED)

add_library(chronolog_common_tier STATIC
    ArchiveReaderPool.cpp
    ChunkCodec.cpp
    HDF5ChunkCodec.cpp
    ProtoChunkCodec.cpp
    PosixTier.cpp
    ManifestLog.cpp
    FileTierStore.cpp)
target_include_directories(chronolog_common_tier PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(chronolog_common_tier PUBLIC
    chronolog_contracts chronolog_proto Threads::Threads
    PRIVATE hdf5::hdf5-static chronolog_warnings nlohmann_json::nlohmann_json absl::log absl::crc32c)

add_executable(chronolog_common_tier_tests tests/file_tier_store_test.cpp)
target_link_libraries(chronolog_common_tier_tests PRIVATE
    chronolog_common_tier chronolog_warnings nlohmann_json::nlohmann_json GTest::gtest GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(chronolog_common_tier_tests DISCOVERY_TIMEOUT 60
    TEST_PREFIX "grapher." DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(chronolog_hdf5_interop_writer tests/hdf5_interop_writer.cpp)
target_link_libraries(chronolog_hdf5_interop_writer PRIVATE chronolog_common_tier chronolog_warnings)
if(CHRONOLOG_BUILD_BENCH)
    add_executable(chronolog_archive_physical_bench tests/physical_read_bench.cpp)
    target_link_libraries(chronolog_archive_physical_bench PRIVATE
        chronolog_common_tier chronolog_warnings nlohmann_json::nlohmann_json)
    add_executable(chronolog_archive_compaction_bench tests/compaction_bench.cpp)
    target_link_libraries(chronolog_archive_compaction_bench PRIVATE chronolog_common_tier chronolog_warnings)
    add_executable(chronolog_archive_recovery_bench tests/recovery_bench.cpp)
    target_link_libraries(chronolog_archive_recovery_bench PRIVATE chronolog_common_tier chronolog_warnings)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    # h5py lives in a venv under build/ (audit B2): the system interpreters have none, and the test never skips.
    set(h5py_venv "${PROJECT_SOURCE_DIR}/build/h5py-venv")
    add_test(NAME grapher.HDF5InteropEnv
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/hdf5_interop_env.sh" "${h5py_venv}")
    set_tests_properties(grapher.HDF5InteropEnv PROPERTIES TIMEOUT 300 FIXTURES_SETUP h5py_venv)
    add_test(NAME grapher.HDF5Interop
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${h5py_venv}/bin/python" "${CMAKE_CURRENT_SOURCE_DIR}/tests/hdf5_interop.py"
            $<TARGET_FILE:chronolog_hdf5_interop_writer>)
    set_tests_properties(grapher.HDF5Interop PROPERTIES TIMEOUT 30 FIXTURES_REQUIRED h5py_venv)
endif()
