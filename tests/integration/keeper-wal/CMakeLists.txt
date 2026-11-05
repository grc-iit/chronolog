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
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/client/cpp/src
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
)

target_link_libraries(durable_probe
    PRIVATE
        chronolog_client
        chrono_common
        thallium
        pthread
        rt
)

set_target_properties(durable_probe PROPERTIES
    OUTPUT_NAME chronolog-test-durable-probe
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# WAL directories the startup test points a real keeper at: a bad record in an
# older segment, and a bad record in the last group with an intact record after it.
add_executable(wal_fixture
    wal_fixture.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/src/WalFormat.cpp
)

target_include_directories(wal_fixture PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(wal_fixture
    PRIVATE
        chrono_common
)

set_target_properties(wal_fixture PROPERTIES
    OUTPUT_NAME chronolog-test-wal-fixture
)

# Holds fdatasync of a WAL segment while a file exists. The shutdown test
# preloads it into the keeper so one durable group stays inside its sync.
add_library(wal_sync_hold SHARED
    wal_sync_hold.c
)

target_link_libraries(wal_sync_hold PRIVATE ${CMAKE_DL_LIBS})

set_target_properties(wal_sync_hold PROPERTIES
    OUTPUT_NAME chronolog_wal_sync_hold
    PREFIX "lib"
)
