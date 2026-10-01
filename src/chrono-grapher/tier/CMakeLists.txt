add_library(chronolog_grapher_tier STATIC
    ProtoChunkCodec.cpp
    ManifestLog.cpp
    FileTierStore.cpp)
target_include_directories(chronolog_grapher_tier PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(chronolog_grapher_tier PUBLIC
    chronolog_contracts chronolog_proto Threads::Threads
    PRIVATE chronolog_warnings nlohmann_json::nlohmann_json)

add_executable(chronolog_grapher_tier_tests ../tests/file_tier_store_test.cpp)
target_link_libraries(chronolog_grapher_tier_tests PRIVATE
    chronolog_grapher_tier chronolog_warnings GTest::gtest GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(chronolog_grapher_tier_tests
    TEST_PREFIX "grapher." DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
