find_package(mercury REQUIRED)
find_package(thallium REQUIRED)
find_package(Threads REQUIRED)

add_executable(chronolog_client_test chronolog_client.cpp)
target_link_libraries(chronolog_client_test Serde socketpp pthread)
add_dependencies(chronolog_client_test Serde socketpp)

add_test(NAME chronolog_client_test COMMAND chronolog_client_test)

add_executable(chronovisor_server_test chronovisor_server.cpp
        ../../../../ChronoVisor/src/ChronoVisorServer2.cpp
        ../../../../ChronoVisor/include/ClocksourceManager.h
        ../../../../ChronoVisor/src/ClocksourceManager.cpp
        ../../../../ChronoAPI/ChronoLog/include/ChronoLogAdminRPCVisor.h
        ../../../../ChronoVisor/include/ClientRegistryManager.h
        ../../../../ChronoVisor/src/ClientRegistryManager.cpp
        ../../../../ChronoVisor/include/ClientRegistryInfo.h
        ../../../../ChronoVisor/src/ClientRegistryRecord.cpp
        ../../../../ChronoAPI/ChronoLog/include/ChronicleMetadataRPCVisor.h
        ../../../../ChronoAPI/ChronoLog/include/macro.h
        ../../../../ChronoAPI/ChronoLog/include/rpc.h
        ../../../../ChronoAPI/ChronoLog/include/RPCFactory.h
        ../../../../ChronoVisor/src/ChronicleMetaDirectory.cpp
        ../../../../ChronoVisor/include/Chronicle.h
        ../../../../ChronoVisor/include/Story.h
        ../../../../ChronoVisor/include/Event.h
        ../../../../ChronoVisor/src/city.cpp
        ../../../../ChronoVisor/include/city.h
        ../../../../tools/Serde/src/SerDe.cpp)
target_include_directories(chronovisor_server_test PRIVATE
        ../../../../ChronoAPI/ChronoLog/include
        ../../../../tools/Serde/include
        ../../../../ChronoVisor/include)
target_link_libraries(chronovisor_server_test thallium)
add_custom_command(TARGET chronovisor_server_test POST_BUILD
        COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/../server_list.in ${CMAKE_CURRENT_BINARY_DIR}/server_list)
add_test(NAME ChronoVisorServerTest COMMAND chronovisor_server_test)