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
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/ChronoKeeper/include
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
