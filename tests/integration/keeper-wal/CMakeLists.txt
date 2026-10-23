cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# Durable and accepted writer and replay probe used by
# tests/integration/keeper_wal_crash_test.sh
#------------------------------------------------------------------------------
add_executable(durable_probe
    durable_probe.cpp
)

target_include_directories(durable_probe PRIVATE
    ${CMAKE_BINARY_DIR}/client/cpp/include
)

target_link_libraries(durable_probe
    PRIVATE
        chronolog_client
        pthread
        rt
)

set_target_properties(durable_probe PROPERTIES
    OUTPUT_NAME chronolog-test-durable-probe
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)
