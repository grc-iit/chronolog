find_package(mercury REQUIRED)
find_package(thallium REQUIRED)

add_executable(chronolog_client_lib_multithreaded_test client_lib_multithreaded_test.cpp ../../include/ChronoLogAdminRPCClient.h)
target_include_directories(chronolog_client_lib_multithreaded_test PRIVATE ../../../Client/include
        ../../../ChronoAPI/ChronoLog/include
        ../../../ChronoVisor/include)
target_link_libraries(chronolog_client_lib_multithreaded_test ChronoLogClient_lib)
target_link_libraries(chronolog_client_lib_multithreaded_test thallium pthread)
add_dependencies(chronolog_client_lib_multithreaded_test ChronoLogClient_lib)
add_test(NAME ClientMultithreadedTest COMMAND chronolog_client_lib_multithreaded_test)