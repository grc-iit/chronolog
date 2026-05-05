cmake_minimum_required(VERSION 3.25)

find_package(Threads REQUIRED)

add_executable(chronosql_integration_test
    chronosql_integration_test.cpp
)

target_include_directories(chronosql_integration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronosql/include
)

target_link_libraries(chronosql_integration_test
    PRIVATE
        chronosql
        Threads::Threads
)

target_compile_features(chronosql_integration_test PRIVATE cxx_std_17)

set_target_properties(chronosql_integration_test PROPERTIES
    OUTPUT_NAME chronolog-test-chronosql-integration
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Integration_ChronoSQL_PluginIntegration
    COMMAND chronosql_integration_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

set_tests_properties(Integration_ChronoSQL_PluginIntegration PROPERTIES
    MANUAL TRUE
    # Cold-start budget: ~180s for the initial metadata replay (the ChronoLog
    # player times out empty-story queries at its hardcoded 180s ceiling),
    # plus 120s for data propagation, plus the SELECT roundtrips.
    TIMEOUT 600
)

if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronosql_integration_test DESTINATION tests)
endif()
