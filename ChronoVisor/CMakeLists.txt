set(CMAKE_FIND_LIBRARY_TYPE STATIC)

find_package(thallium REQUIRED)
find_package(Threads REQUIRED)

add_executable(chronovisor_server)

target_sources(chronovisor_server PRIVATE
    ./src/chronovisor_instance.cpp
    ./src/VisorClientPortal.cpp
    #        ../ChronoAPI/ChronoLog/src/ClocksourceManager.cpp
    ./src/ClientRegistryManager.cpp
    ./src/ClientRegistryRecord.cpp
    ./src/ChronicleMetaDirectory.cpp
    ./src/KeeperRegistry.cpp
    ../ChronoAPI/ChronoLog/src/city.cpp
    ../ChronoAPI/ChronoLog/src/log.cpp)

target_include_directories(chronovisor_server PRIVATE
    ../ChronoAPI/ChronoLog/include
    ../chrono_common
    ./include)

target_link_libraries(chronovisor_server chronolog_client thallium)

# prep the stage...
configure_file(${CMAKE_SOURCE_DIR}/default_conf.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)
add_test(NAME ChronoVisorServerTest COMMAND chronovisor_server)

set_target_properties(chronovisor_server PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# install binary
install(
    TARGETS chronovisor_server DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in DESTINATION conf RENAME default_conf.json
)
