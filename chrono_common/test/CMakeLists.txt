cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# StoryChunkExtractionChain Test
#------------------------------------------------------------------------------
add_executable(extraction_chain_test
    ExtractionChainTest.cpp
)

target_include_directories(extraction_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(extraction_chain_test
    PRIVATE
        chrono_common
        chronolog_client
        thallium
)

set_target_properties(extraction_chain_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Standalone StoryChunkConsumerService Test
#------------------------------------------------------------------------------
add_executable(chunk_consumer_service_test
    ChunkConsumerServiceTest.cpp
)

target_include_directories(chunk_consumer_service_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(chunk_consumer_service_test
    PRIVATE
        chrono_common
        chronolog_client
        thallium
)

set_target_properties(chunk_consumer_service_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Install test executables to tests/
#------------------------------------------------------------------------------
chronolog_install_target(extraction_chain_test DESTINATION tests)
chronolog_install_target(chunk_consumer_service_test DESTINATION tests)
