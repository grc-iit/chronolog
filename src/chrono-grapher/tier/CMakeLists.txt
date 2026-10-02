find_package(hdf5 CONFIG REQUIRED)

add_library(chronolog_grapher_tier STATIC
    HDF5ChunkCodec.cpp
    ProtoChunkCodec.cpp
    ManifestLog.cpp
    FileTierStore.cpp)
target_include_directories(chronolog_grapher_tier PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(chronolog_grapher_tier PUBLIC
    chronolog_contracts chronolog_proto Threads::Threads
    PRIVATE hdf5::hdf5-static chronolog_warnings nlohmann_json::nlohmann_json absl::log)

add_executable(chronolog_grapher_tier_tests ../tests/file_tier_store_test.cpp)
target_link_libraries(chronolog_grapher_tier_tests PRIVATE
    chronolog_grapher_tier chronolog_warnings GTest::gtest GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(chronolog_grapher_tier_tests DISCOVERY_TIMEOUT 60
    TEST_PREFIX "grapher." DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)

add_executable(chronolog_hdf5_interop_writer ../tests/hdf5_interop_writer.cpp)
target_link_libraries(chronolog_hdf5_interop_writer PRIVATE chronolog_grapher_tier chronolog_warnings)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME grapher.HDF5Interop
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/hdf5_interop.py"
            $<TARGET_FILE:chronolog_hdf5_interop_writer>)
    set_tests_properties(grapher.HDF5Interop PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endif()
