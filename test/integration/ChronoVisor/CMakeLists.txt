find_package(mercury REQUIRED)
find_package(thallium REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_INSTALL_RPATH /home/chronolog/chronolog/bin/*)

add_executable(chronovisor_server_test chronovisor_server.cpp
        ../../../ChronoVisor/src/ChronoVisorServer2.cpp
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
target_link_libraries(chronovisor_server_test thallium)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)
add_test(NAME ChronoVisorServerTest COMMAND chronovisor_server_test)

# get all dependencies
execute_process(
        COMMAND sh -c "ldd ${CMAKE_CURRENT_BINARY_DIR}/chronovisor_server_test \
                       | awk '{print $3}' | grep -v -e \"^$\\|^/lib\\|not\""
        OUTPUT_VARIABLE DEPENDENCIES
)
message("Dependencies:")
message("${DEPENDENCIES}")

# convert them into a list
string(REPLACE "\n" ";" DEPENDENCY_LIST "${DEPENDENCIES}")
list(LENGTH DEPENDENCY_LIST len)
message("Dependency list (size: ${len}):")
message("${DEPENDENCY_LIST}")

# copy each dependency to destination directory
foreach (dependency IN LISTS DEPENDENCY_LIST)
        #if (dependency STREQUAL "")
                message("Installing dependency: ${dependency} ...")
                execute_process(COMMAND cp -u -f ${dependency} ${CMAKE_INSTALL_PREFIX}/bin/)
        #endif ()
endforeach ()
execute_process(COMMAND sh -c "chmod 777 -f ${CMAKE_INSTALL_PREFIX}/bin/*")

# install binary
install(
        TARGETS chronovisor_server_test DESTINATION bin
)

# install configuration file
install(
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in DESTINATION bin RENAME default_conf.json
)
