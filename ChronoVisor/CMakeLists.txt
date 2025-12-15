cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)

set(CMAKE_FIND_LIBRARY_TYPE STATIC)

#------------------------------------------------------------------------------
# ChronoVisor Executable
#------------------------------------------------------------------------------
add_executable(chronovisor_server)

target_include_directories(chronovisor_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_sources(chronovisor_server PRIVATE
    src/chronovisor_instance.cpp
    src/ChronoVisorConfiguration.cpp
    src/VisorClientPortal.cpp
    src/ClientRegistryManager.cpp
    src/ClientRegistryRecord.cpp
    src/ChronicleMetaDirectory.cpp
    src/KeeperRegistry.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/city.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

target_link_libraries(chronovisor_server
    PRIVATE
        chronolog_client
        thallium
)

set_target_properties(chronovisor_server PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

#------------------------------------------------------------------------------
# Install binary and configuration file
#------------------------------------------------------------------------------
chronolog_install_target(chronovisor_server)

install(
    FILES ${CMAKE_SOURCE_DIR}/default_conf.json.in
    DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR}
    RENAME default_conf.json
)
