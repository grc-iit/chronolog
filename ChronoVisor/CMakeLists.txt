cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

#message("Chronovisor CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("Chronovisor CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : chronovisor_server")

set(CMAKE_FIND_LIBRARY_TYPE STATIC)

add_executable(chronovisor_server)

target_sources(chronovisor_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/chronovisor_instance.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/VisorClientPortal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientRegistryManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientRegistryRecord.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ChronicleMetaDirectory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/KeeperRegistry.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/city.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp)

target_include_directories(chronovisor_server PRIVATE
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(chronovisor_server chronolog_client thallium)

#add_test(NAME ChronoVisorServerTest COMMAND chronovisor_server)

set_target_properties(chronovisor_server PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# Install binary
install(
    TARGETS chronovisor_server DESTINATION ${CHRONOLOG_INSTALL_BIN_DIR}
)

# Install configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_conf.json.in DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR} RENAME default_conf.json
)
