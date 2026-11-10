cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(MPI REQUIRED)

#------------------------------------------------------------------------------
# Build Configuration
#------------------------------------------------------------------------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Compiling communication test in Debug Mode")
    add_compile_options(-O0)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0")
endif()

#------------------------------------------------------------------------------
# Test Executables
#------------------------------------------------------------------------------

# Thallium Server Test
add_executable(thallium_server
    thallium_server_test.cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/src/chrono_monitor.cpp
)

target_include_directories(thallium_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(thallium_server
    PRIVATE
        thallium
        pthread
)

add_test(NAME Communication_Thallium_ServerRepeaterRpc COMMAND thallium_server)

# Thallium Client Test
add_executable(thallium_client
    thallium_client_test.cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/src/chrono_monitor.cpp
)

target_include_directories(thallium_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(thallium_client
    PRIVATE
        thallium
)

add_test(NAME Communication_Thallium_ClientSendRecvOrRdma COMMAND thallium_client)
set_tests_properties(Communication_Thallium_ClientSendRecvOrRdma PROPERTIES MANUAL TRUE)

# Thallium Client MPI Test
add_executable(thallium_client_mpi
    thallium_client_mpi_test.cpp
    ${CMAKE_SOURCE_DIR}/client/cpp/src/chrono_monitor.cpp
)

target_include_directories(thallium_client_mpi SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include
    ${MPI_INCLUDE_PATH}
)

target_link_libraries(thallium_client_mpi
    PRIVATE
        thallium
        ${MPI_C_LIBRARIES}
        ${MPI_CXX_LIBRARIES}
)

add_test(NAME Communication_Thallium_ClientMpi COMMAND thallium_client_mpi)

# Thallium protocol multi-node script (SLURM/squeue; run manually)
add_test(NAME Communication_Thallium_ProtocolScript COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/thallium_protocol_test.sh)
set_tests_properties(Communication_Thallium_ProtocolScript PROPERTIES MANUAL TRUE)

# Self-contained bounded-RPC capability peers; unsupported transports fail.
add_executable(rpc_deadline_capability_probe rpc_deadline_capability_probe.cpp)
target_link_libraries(rpc_deadline_capability_probe PRIVATE thallium pthread)
foreach(transport IN ITEMS sockets tcp)
    add_test(NAME Communication_RpcDeadlineCapability_${transport}
        COMMAND rpc_deadline_capability_probe --primitives ofi+${transport})
    set_tests_properties(Communication_RpcDeadlineCapability_${transport}
        PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
endforeach()

add_executable(rpc_bulk_lifetime_test rpc_bulk_lifetime_test.cpp)
target_include_directories(rpc_bulk_lifetime_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(rpc_bulk_lifetime_test PRIVATE chrono_common chronolog_client chrono_rpc thallium pthread)
foreach(transport IN ITEMS sockets tcp)
    add_test(NAME Communication_RpcBulkLifetime_${transport}
        COMMAND rpc_bulk_lifetime_test ofi+${transport})
    set_tests_properties(Communication_RpcBulkLifetime_${transport}
        PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endforeach()

# A keeper partial chunk through the grapher's real receive handler: pieces,
# checksum, duplicate, pull-error (see the scenario list in the source).
add_executable(rpc_chunk_receive_test rpc_chunk_receive_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/src/WatermarkReportPublisher.cpp)
target_include_directories(rpc_chunk_receive_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(rpc_chunk_receive_test PRIVATE chrono_common chronolog_client chrono_rpc thallium pthread)
foreach(transport IN ITEMS sockets tcp)
    foreach(scenario IN ITEMS pieces checksum duplicate pull-error)
        add_test(NAME Communication_RpcChunkReceive_${transport}_${scenario}
            COMMAND rpc_chunk_receive_test ofi+${transport} ${scenario})
        set_tests_properties(Communication_RpcChunkReceive_${transport}_${scenario}
            PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE ENVIRONMENT "FI_TCP_IFACE=lo")
    endforeach()
endforeach()

add_executable(rpc_bulk_receiver_test rpc_bulk_receiver_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/src/WatermarkReportPublisher.cpp)
target_include_directories(rpc_bulk_receiver_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-grapher/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(rpc_bulk_receiver_test PRIVATE chrono_common chronolog_client chrono_rpc thallium pthread)
foreach(transport IN ITEMS sockets tcp)
    foreach(scenario IN ITEMS capacity reply-error decode-error failure-reply-error)
        add_test(NAME Communication_RpcBulkReceiver_${transport}_${scenario}
            COMMAND rpc_bulk_receiver_test ofi+${transport} ${scenario})
        set_tests_properties(Communication_RpcBulkReceiver_${transport}_${scenario}
            PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
    endforeach()
endforeach()

add_executable(rpc_player_response_test rpc_player_response_test.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/PlaybackService.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/QueryResponseTransferAgent.cpp
    ${CMAKE_SOURCE_DIR}/src/chrono-player/src/PlayerDataStore.cpp)
target_include_directories(rpc_player_response_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/chrono-player/include
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(rpc_player_response_test PRIVATE chrono_common chronolog_client chrono_rpc thallium pthread)
foreach(transport IN ITEMS sockets tcp)
    add_test(NAME Communication_RpcPlayerResponse_${transport}
        COMMAND rpc_player_response_test ofi+${transport})
    set_tests_properties(Communication_RpcPlayerResponse_${transport}
        PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
endforeach()

add_executable(rpc_client_bulk_test rpc_client_bulk_test.cpp)
target_include_directories(rpc_client_bulk_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/src
    ${CMAKE_SOURCE_DIR}/src/chrono-common/include
    ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(rpc_client_bulk_test PRIVATE chronolog_client chrono_common chrono_rpc thallium pthread)
foreach(transport IN ITEMS sockets tcp)
    foreach(scenario IN ITEMS register-before register-after response-before-ack late-private staged-expiry callback-thread callback-active callback-late callback-throw callback-control)
        add_test(NAME Communication_RpcClientBulk_${transport}_${scenario}
            COMMAND rpc_client_bulk_test ofi+${transport} ${scenario})
        set_tests_properties(Communication_RpcClientBulk_${transport}_${scenario}
            PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endforeach()
endforeach()

foreach(transport IN ITEMS sockets tcp)
    foreach(scenario IN ITEMS identity same-range route-key capacity prep-expired queued during default large-chunk empty)
        add_test(NAME Communication_RpcBulkLifetime_${transport}_${scenario}
            COMMAND rpc_bulk_lifetime_test ofi+${transport} ${scenario})
        set_tests_properties(Communication_RpcBulkLifetime_${transport}_${scenario}
            PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    endforeach()
endforeach()
