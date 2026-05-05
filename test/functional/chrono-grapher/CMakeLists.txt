cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)

find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# chrono_grapher standalone tests (Thallium/Margo)
#------------------------------------------------------------------------------

add_executable(chrono_grapher_extraction_chain_test
    chrono_grapher_extraction_chain_test.cpp
    ${CMAKE_SOURCE_DIR}/ChronoGrapher/src/HDF5FileChunkExtractor.cpp
)

target_include_directories(chrono_grapher_extraction_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/ChronoGrapher/include
    ${HDF5_INCLUDE_DIRS}
)

target_link_libraries(chrono_grapher_extraction_chain_test PRIVATE
    chrono_common
    chronolog_client
    thallium
    ${HDF5_LIBRARIES}
)

set_target_properties(chrono_grapher_extraction_chain_test PROPERTIES
    OUTPUT_NAME chronolog-test-grapher-extraction-chain
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Functional_ChronoGrapher_ExtractionChain COMMAND chrono_grapher_extraction_chain_test)
set_tests_properties(Functional_ChronoGrapher_ExtractionChain PROPERTIES DISABLED TRUE)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chrono_grapher_extraction_chain_test DESTINATION tests)
endif()
