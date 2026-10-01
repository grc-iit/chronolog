add_library(chrono_player_replay STATIC
    replay/CompletionPolicy.cpp
    replay/HotReplay.cpp
    replay/ReplayMerge.cpp)
target_include_directories(chrono_player_replay PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(chrono_player_replay PUBLIC chronolog_contracts chronolog_proto Threads::Threads
    PRIVATE chronolog_warnings)

include(GoogleTest)

function(chrono_player_test name source prefix)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/tests/contract")
    target_link_libraries(${name} PRIVATE chrono_player_replay chronolog_warnings GTest::gtest GTest::gtest_main)
    if("thread" IN_LIST CHRONOLOG_SANITIZERS AND CHRONOLOG_SETARCH)
        set_property(TARGET ${name} PROPERTY CROSSCOMPILING_EMULATOR
            "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()
    gtest_discover_tests(${name} TEST_PREFIX "${prefix}" DISCOVERY_MODE PRE_TEST)
endfunction()

find_program(CHRONOLOG_SETARCH setarch)
chrono_player_test(chrono_player_replay_merge_test tests/replay_merge_test.cpp "replay_merge.")
chrono_player_test(chrono_player_completion_policy_test tests/completion_policy_test.cpp "completion_policy.")
chrono_player_test(chrono_player_hot_replay_contract_test tests/hot_replay_contract.cpp "")
