# Shared code (M11.2): the clock, the gRPC channel layer (M11.3), the worker pool, the tier store and the predicate matcher.
find_package(hdf5 CONFIG REQUIRED)
include(GoogleTest)

add_library(chronolog_common_clock STATIC
    clock/SystemClock.cpp clock/KernelClock.cpp clock/ClockAudit.cpp clock/VisorClockAudit.cpp)
target_include_directories(chronolog_common_clock PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
target_link_libraries(chronolog_common_clock PUBLIC chronolog_contracts absl::status absl::statusor
    PRIVATE chronolog_warnings absl::log Threads::Threads)

add_library(chronolog_common_rpc INTERFACE)
target_include_directories(chronolog_common_rpc INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
target_link_libraries(chronolog_common_rpc INTERFACE gRPC::grpc++ chronolog_contracts)

add_library(chronolog_common_worker STATIC worker/WorkerPool.cpp)
target_include_directories(chronolog_common_worker PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
target_compile_features(chronolog_common_worker PUBLIC cxx_std_20)
target_link_libraries(chronolog_common_worker PUBLIC Threads::Threads PRIVATE chronolog_warnings)

add_library(chronolog_common_tier STATIC
    tier/ArchiveReaderPool.cpp
    tier/ChunkCodec.cpp
    tier/HDF5ChunkCodec.cpp
    tier/ProtoChunkCodec.cpp
    tier/PosixTier.cpp
    tier/ManifestLog.cpp
    tier/FileTierStore.cpp)
target_include_directories(chronolog_common_tier PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
target_link_libraries(chronolog_common_tier PUBLIC
    chronolog_contracts chronolog_proto Threads::Threads
    PRIVATE hdf5::hdf5-static chronolog_warnings nlohmann_json::nlohmann_json absl::log absl::crc32c)

add_library(chronolog_common_predicate STATIC predicate/Predicate.cpp)
target_include_directories(chronolog_common_predicate PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
target_link_libraries(chronolog_common_predicate PUBLIC chronolog_contracts PRIVATE chronolog_warnings)

add_executable(chronolog_common_clock_tests
    clock/tests/clock_audit_test.cpp clock/tests/kernel_clock_test.cpp clock/tests/visor_clock_audit_test.cpp)
target_link_libraries(chronolog_common_clock_tests PRIVATE chronolog_common_clock chronolog_warnings
    absl::log_sink absl::log_sink_registry GTest::gtest GTest::gtest_main Threads::Threads)
gtest_discover_tests(chronolog_common_clock_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "common."
    DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(chronolog_common_rpc_tests rpc/channel_pool_test.cpp)
target_link_libraries(chronolog_common_rpc_tests PRIVATE
    chronolog_common_rpc ${CMAKE_DL_LIBS} chronolog_internal_proto chronolog_warnings GTest::gtest GTest::gtest_main)
gtest_discover_tests(chronolog_common_rpc_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "common."
    DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)

add_executable(chronolog_common_worker_tests worker/worker_pool_test.cpp)
target_link_libraries(chronolog_common_worker_tests PRIVATE
    chronolog_common_worker chronolog_warnings GTest::gtest GTest::gtest_main)
gtest_discover_tests(chronolog_common_worker_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "common."
    DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(chronolog_common_tier_tests tier/tests/file_tier_store_test.cpp)
target_include_directories(chronolog_common_tier_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests/contract")
target_link_libraries(chronolog_common_tier_tests PRIVATE
    chronolog_common_tier chronolog_warnings nlohmann_json::nlohmann_json GTest::gtest GTest::gtest_main)
gtest_discover_tests(chronolog_common_tier_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "common."
    DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(chronolog_common_predicate_tests predicate/tests/predicate_test.cpp)
target_link_libraries(chronolog_common_predicate_tests PRIVATE
    chronolog_common_predicate chronolog_warnings GTest::gtest GTest::gtest_main)
gtest_discover_tests(chronolog_common_predicate_tests DISCOVERY_TIMEOUT 60 TEST_PREFIX "common."
    DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(chronolog_hdf5_interop_writer tier/tests/hdf5_interop_writer.cpp)
target_link_libraries(chronolog_hdf5_interop_writer PRIVATE chronolog_common_tier chronolog_warnings)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    # h5py lives in a venv under build/ (audit B2): the system interpreters have none, and the test never skips.
    set(h5py_venv "${PROJECT_SOURCE_DIR}/build/h5py-venv")
    add_test(NAME common.HDF5InteropEnv
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tier/tests/hdf5_interop_env.sh" "${h5py_venv}")
    set_tests_properties(common.HDF5InteropEnv PROPERTIES TIMEOUT 300 FIXTURES_SETUP h5py_venv)
    add_test(NAME common.HDF5Interop
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${h5py_venv}/bin/python"
            "${CMAKE_CURRENT_SOURCE_DIR}/tier/tests/hdf5_interop.py" $<TARGET_FILE:chronolog_hdf5_interop_writer>)
    set_tests_properties(common.HDF5Interop PROPERTIES TIMEOUT 30 FIXTURES_REQUIRED h5py_venv)
endif()
