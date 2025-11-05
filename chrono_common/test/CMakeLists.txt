cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)
find_package(HDF5 REQUIRED COMPONENTS C CXX)

#### TransferAgentTest
add_executable(extraction_chain_test
    ExtractionChainTest.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ChunkExtractorCSV.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/RDMATransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ChunkExtractorRDMA.cpp
    )

target_include_directories(extraction_chain_test PRIVATE
    include
)
target_link_libraries(extraction_chain_test chronolog_client thallium)

set_target_properties(extraction_chain_test PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)


# install executables
chronolog_install_target(extraction_chain_test)

