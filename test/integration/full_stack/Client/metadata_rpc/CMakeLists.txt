find_package(mercury REQUIRED)
find_package(thallium REQUIRED)

add_executable(chronolog_client_lib_metadata_rpc_test client_lib_metadata_rpc_test.cpp)
target_include_directories(chronolog_client_lib_metadata_rpc_test PRIVATE ../../../../../Client/include
        ../../../../../ChronoAPI/ChronoLog/include
        ../../../../../ChronoVisor/include)
target_link_libraries(chronolog_client_lib_metadata_rpc_test ChronoLogClient_lib)
target_link_libraries(chronolog_client_lib_metadata_rpc_test thallium)
add_dependencies(chronolog_client_lib_metadata_rpc_test ChronoLogClient_lib)
add_custom_command(TARGET chronolog_client_lib_metadata_rpc_test POST_BUILD
        COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/../../server_list.in ${CMAKE_CURRENT_BINARY_DIR}/server_list)
add_test(NAME ClientMetadataRPCTest COMMAND chronolog_client_lib_metadata_rpc_test)