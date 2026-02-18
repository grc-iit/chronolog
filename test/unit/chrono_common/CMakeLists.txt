cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# chrono_common unit tests (standalone executables)
#------------------------------------------------------------------------------

add_executable(extraction_chain_test
    ExtractionChainTest.cpp
)

target_include_directories(extraction_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(extraction_chain_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(extraction_chain_test PROPERTIES
    OUTPUT_NAME chronolog-test-common-extraction-chain
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Unit_ChronoCommon_ExtractionChain COMMAND extraction_chain_test)
set_tests_properties(Unit_ChronoCommon_ExtractionChain PROPERTIES DISABLED TRUE)
chronolog_install_target(extraction_chain_test DESTINATION tests)

#------------------------------------------------------------------------------

add_executable(chunk_consumer_service_test
    ChunkConsumerServiceTest.cpp
)

target_include_directories(chunk_consumer_service_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

target_link_libraries(chunk_consumer_service_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(chunk_consumer_service_test PROPERTIES
    OUTPUT_NAME chronolog-test-common-chunk-consumer-service
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Unit_ChronoCommon_ChunkConsumerService COMMAND chunk_consumer_service_test)
set_tests_properties(Unit_ChronoCommon_ChunkConsumerService PROPERTIES DISABLED TRUE)
chronolog_install_target(chunk_consumer_service_test DESTINATION tests)
