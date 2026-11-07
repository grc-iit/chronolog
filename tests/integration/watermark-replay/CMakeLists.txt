cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# Replay probe used by tests/integration/watermark_replay_split_test.sh
#------------------------------------------------------------------------------
add_executable(replay_split_check
    replay_split_check.cpp
)

target_include_directories(replay_split_check PRIVATE
    ${CMAKE_BINARY_DIR}/client/cpp/include
)

target_link_libraries(replay_split_check
    PRIVATE
        chronolog_client
        pthread
        rt
)

set_target_properties(replay_split_check PROPERTIES
    OUTPUT_NAME chronolog-test-replay-split-check
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Writer and replay probe used by tests/integration/watermark_exact_split_test.sh
#------------------------------------------------------------------------------
add_executable(exact_split_probe
    exact_split_probe.cpp
)

target_include_directories(exact_split_probe PRIVATE
    ${CMAKE_BINARY_DIR}/client/cpp/include
)

target_link_libraries(exact_split_probe
    PRIVATE
        chronolog_client
        pthread
        rt
)

set_target_properties(exact_split_probe PROPERTIES
    OUTPUT_NAME chronolog-test-exact-split-probe
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

# Public API probe for paused-Keeper and bounded lifecycle integration checks.
add_executable(rpc_lifecycle_probe rpc_lifecycle_probe.cpp)
target_link_libraries(rpc_lifecycle_probe PRIVATE chronolog_client thallium)
set_target_properties(rpc_lifecycle_probe PROPERTIES OUTPUT_NAME chronolog-test-rpc-lifecycle-probe
    BUILD_WITH_INSTALL_RPATH TRUE SKIP_BUILD_RPATH TRUE)

# Only preloaded by the owned native lifecycle recovery fixture.
find_package(HDF5 REQUIRED COMPONENTS C CXX)
add_library(rpc_archive_write_hold SHARED rpc_archive_write_hold.cpp)
target_include_directories(rpc_archive_write_hold PRIVATE ${HDF5_INCLUDE_DIRS})
target_link_libraries(rpc_archive_write_hold PRIVATE ${CMAKE_DL_LIBS})

add_executable(rpc_lifecycle_archive_probe rpc_lifecycle_archive_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/HDF5ArchiveReadingAgent.cpp)
target_include_directories(rpc_lifecycle_archive_probe PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-player/include ${HDF5_INCLUDE_DIRS})
target_link_libraries(rpc_lifecycle_archive_probe PRIVATE
    chrono_common chronolog_client thallium ${HDF5_LIBRARIES})
set_target_properties(rpc_lifecycle_archive_probe PROPERTIES
    OUTPUT_NAME chronolog-test-rpc-lifecycle-archive-probe
    BUILD_WITH_INSTALL_RPATH TRUE SKIP_BUILD_RPATH TRUE)

add_library(rpc_player_init_hold SHARED rpc_player_init_hold.cpp)
target_include_directories(rpc_player_init_hold PRIVATE $<TARGET_PROPERTY:thallium,INTERFACE_INCLUDE_DIRECTORIES>)
target_link_libraries(rpc_player_init_hold PRIVATE ${CMAKE_DL_LIBS})
