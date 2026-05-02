cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
find_package(Thallium REQUIRED)
include(GoogleTest)

find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# chrono_keeper standalone tests (Thallium/Margo)
#------------------------------------------------------------------------------

add_executable(chrono_keeper_extraction_chain_test
    chrono_keeper_extraction_chain_test.cpp
)

target_include_directories(chrono_keeper_extraction_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${CMAKE_SOURCE_DIR}/ChronoKeeper/include
)

target_link_libraries(chrono_keeper_extraction_chain_test PRIVATE
    chrono_common
    chronolog_client
    thallium
)

set_target_properties(chrono_keeper_extraction_chain_test PROPERTIES
    OUTPUT_NAME chronolog-test-keeper-extraction-chain
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

add_test(NAME Functional_ChronoKeeper_ExtractionChain COMMAND chrono_keeper_extraction_chain_test)
set_tests_properties(Functional_ChronoKeeper_ExtractionChain PROPERTIES DISABLED TRUE)
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chrono_keeper_extraction_chain_test DESTINATION tests)
endif()
