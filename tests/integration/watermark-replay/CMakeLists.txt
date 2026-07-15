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
