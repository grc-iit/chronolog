find_package(thallium REQUIRED)
find_package(Threads REQUIRED)

add_executable(chronovisor_server)

target_sources ( chronovisor_server PRIVATE
	chronovisor_server.cpp
        ./src/ChronoVisorServer2.cpp
	#../ChronoAPI/ChronoLog/src/ClocksourceManager.cpp
        ./src/ClientRegistryManager.cpp
        ./src/ClientRegistryRecord.cpp
        ./src/ChronicleMetaDirectory.cpp
	./src/KeeperRegistry.cpp
        ../ChronoAPI/ChronoLog/src/city.cpp)

target_include_directories(chronovisor_server PRIVATE
        ../ChronoAPI/ChronoLog/include
	../chrono_common
        ./include)

target_link_libraries(chronovisor_server chronolog_client thallium)

# prep the stage...
configure_file(${CMAKE_SOURCE_DIR}/default_conf.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)
add_test(NAME ChronoVisorServerTest COMMAND chronovisor_server)

