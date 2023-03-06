find_package(mercury REQUIRED)
find_package(thallium REQUIRED)
find_package(Threads REQUIRED)

add_executable(chronolog_client_test chronolog_client.cpp)
target_link_libraries(chronolog_client_test Serde socketpp pthread)
add_dependencies(chronolog_client_test Serde socketpp)

add_test(NAME chronolog_client_test COMMAND chronolog_client_test)

add_executable(chronovisor_server_test chronovisor_server.cpp
        ../../../ChronoVisor/src/ChronoVisorServer2.cpp
	../../../ChronoAPI/ChronoLog/src/ACL_List.cpp
        ../../../ChronoAPI/ChronoLog/src/ClocksourceManager.cpp
        ../../../ChronoVisor/src/ClientRegistryManager.cpp
        ../../../ChronoVisor/src/ClientRegistryRecord.cpp
        ../../../ChronoVisor/src/ChronicleMetaDirectory.cpp
        ../../../ChronoAPI/ChronoLog/src/city.cpp
        ../../../tools/Serde/src/SerDe.cpp)
target_include_directories(chronovisor_server_test PRIVATE
        ../../../ChronoAPI/ChronoLog/include
        ../../../tools/Serde/include
        ../../../ChronoVisor/include)
target_link_libraries(chronovisor_server_test thallium -ljson-c)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../clients.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/clients.json COPYONLY)
add_test(NAME ChronoVisorServerTest COMMAND chronovisor_server_test)
