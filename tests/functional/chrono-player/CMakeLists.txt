cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

#find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# chrono_keeper standalone tests (Thallium/Margo)
#------------------------------------------------------------------------------

add_executable(chronolog_test_story_chunk_consumer
    story_chunk_consumer_service_test.cpp
)

target_include_directories(chronolog_test_story_chunk_consumer PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${CMAKE_SOURCE_DIR}/src/chrono-keeper/include
)

target_link_libraries(chronolog_test_story_chunk_consumer PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(chronolog_test_story_chunk_consumer PROPERTIES
    OUTPUT_NAME chronolog-test-story-chunk-consumer
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Functional_ChronoPlayer_ChunkConsumer COMMAND chronolog_test_story_chunk_consumer)
set_tests_properties(Functional_ChronoPlayer_ChunkConsumer PROPERTIES DISABLED TRUE)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_test_story_chunk_consumer DESTINATION tests)
endif()

####

add_executable(transfer_agent_test
    chrono_player_transfer_agent_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/QueryResponseTransferAgent.cpp
)

target_include_directories(transfer_agent_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-player/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(transfer_agent_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(transfer_agent_test PROPERTIES
    OUTPUT_NAME chronolog-test-player-transfer-agent
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Functional_ChronoPlayer_QueryResponseAgent COMMAND transfer_agent_test)
set_tests_properties(Functional_ChronoPlayer_QueryResponseAgent PROPERTIES DISABLED TRUE)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(transfer_agent_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------

add_executable(playback_service_test
    chrono_player_playback_service_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/PlaybackService.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/QueryResponseTransferAgent.cpp
)

target_include_directories(playback_service_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-player/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(playback_service_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(playback_service_test PROPERTIES
    OUTPUT_NAME chronolog-test-player-playback-service
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Functional_ChronoPlayer_PlaybackServiceLifecycle COMMAND playback_service_test)
set_tests_properties(Functional_ChronoPlayer_PlaybackServiceLifecycle PROPERTIES DISABLED TRUE)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(playback_service_test DESTINATION tests)
endif()

