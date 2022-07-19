find_package(mercury REQUIRED)
find_package(thallium REQUIRED)

add_executable(chronolog_client_lib_connect_rpc_test client_lib_connect_rpc_test.cpp)
target_include_directories(chronolog_client_lib_connect_rpc_test PRIVATE ../../../Client/include
        ../../../ChronoAPI/ChronoLog/include
        ../../../ChronoVisor/include)
target_link_libraries(chronolog_client_lib_connect_rpc_test ChronoLogClient_lib)
target_link_libraries(chronolog_client_lib_connect_rpc_test thallium)
add_dependencies(chronolog_client_lib_connect_rpc_test ChronoLogClient_lib)
add_test(NAME ClientConnectRPCTest COMMAND chronolog_client_lib_connect_rpc_test)