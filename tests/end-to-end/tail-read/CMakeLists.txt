cmake_minimum_required(VERSION 3.25)

#------------------------------------------------------------------------------
# End-to-end tail-read (playback) test.
#
# Connects to the running stack (the CI pipeline's Deploy step, or a local
# deploy_local.sh --start), writes N known events to a fresh story, and polls
# StoryHandle::playback(N) until the keeper tail returns them — asserting the
# last-N come back in ascending order with matching payloads. Exercises the real
# two-phase tail-read RPC path across the deployed keepers, which the
# header-only KeeperTailStore unit test cannot cover.
#
# Like the data-integrity group, this assumes a stack is already running; the
# test self-injects and self-cleans, so no ctest fixture is required. The
# ctest step in the local pipeline runs after Deploy, so the stack is up.
#------------------------------------------------------------------------------

add_executable(tail_read_playback_test
    tail_read_playback_test.cpp
)

target_include_directories(tail_read_playback_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_BINARY_DIR}/client/cpp/include
)

target_link_libraries(tail_read_playback_test PRIVATE
    chronolog_client
    chrono_common
    pthread
    rt
)

set_target_properties(tail_read_playback_test PROPERTIES
    OUTPUT_NAME chronolog-test-e2e-tail-read
)

add_test(NAME EndToEnd_TailRead_Playback
    COMMAND tail_read_playback_test --events 25 --max-wait 120)
set_tests_properties(EndToEnd_TailRead_Playback PROPERTIES TIMEOUT 180)

if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(tail_read_playback_test DESTINATION tests)
endif()
