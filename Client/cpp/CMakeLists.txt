cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

# json-c is needed because ClientConfiguration.h (public header) includes json-c/json.h
# PkgConfig is already found in root CMakeLists.txt, but we check for json-c here
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON-C REQUIRED json-c)

#------------------------------------------------------------------------------
# chronolog_client Library
#------------------------------------------------------------------------------
add_library(chronolog_client
    include/chronolog_client.h
    src/ChronologClientImpl.h
    src/KeeperRecordingClient.h
    src/StorytellerClient.h
    src/StorytellerClient.cpp
    src/PlaybackQueryRpcClient.cpp
    src/ClientQueryService.cpp
    src/ChronologClient.cpp
    src/ChronologClientImpl.cpp
    src/ClientConfiguration.cpp
    ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
)

# Include directories for the library
# PUBLIC: Headers needed by consumers of this library
# Use generator expressions to handle build vs install interfaces
target_include_directories(chronolog_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CHRONOLOG_INSTALL_INCLUDE_DIR}>
        # json-c is PUBLIC because public headers include json-c/json.h
        ${JSON-C_INCLUDE_DIRS}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/chrono_common/include
        ${CMAKE_SOURCE_DIR}/Client/cpp/include
)

# Link dependencies
# thallium is PRIVATE - it's an implementation detail, not part of the public API
# json-c is PRIVATE - only library implementation needs to link, consumers just need headers
target_link_directories(chronolog_client
    PRIVATE
        ${JSON-C_LIBRARY_DIRS}
)

target_link_libraries(chronolog_client
    PRIVATE
        chrono_common
        thallium
        ${JSON-C_LIBRARIES}
)

# spdlog is PRIVATE: public headers no longer expose spdlog types
if(TARGET spdlog::spdlog)
    target_link_libraries(chronolog_client
        PRIVATE
            spdlog::spdlog
    )
else()
    target_include_directories(chronolog_client
        PRIVATE
            ${spdlog_INCLUDE_DIRS}
    )
endif()

# Define the header files to be copied
set(PUBLIC_HEADER_FILES
    include/chronolog_client.h
    include/ClientConfiguration.h
    include/client_errcode.h
    include/log_level.h
    include/chrono_monitor.h
)

# Set PUBLIC_HEADER property for automatic installation
set_target_properties(chronolog_client PROPERTIES
    PUBLIC_HEADER "${PUBLIC_HEADER_FILES}"
)

#------------------------------------------------------------------------------
# Install library, headers, and configuration file
#------------------------------------------------------------------------------
# Install chronolog_client with export (chrono_common is not installed as it's
# only used internally by server-side ChronoLog components)
install(TARGETS chronolog_client
    EXPORT ChronologTargets
    RUNTIME DESTINATION ${CHRONOLOG_INSTALL_BIN_DIR}
    LIBRARY DESTINATION ${CHRONOLOG_INSTALL_LIB_DIR}
    ARCHIVE DESTINATION ${CHRONOLOG_INSTALL_LIB_DIR}
    PUBLIC_HEADER DESTINATION ${CHRONOLOG_INSTALL_INCLUDE_DIR}
)
# Set both absolute and relative RPATH for portability
set_target_properties(chronolog_client PROPERTIES INSTALL_RPATH "${CHRONOLOG_INSTALL_LIB_DIR};$ORIGIN/../lib")

# Export target for CMake config files
install(EXPORT ChronologTargets
    FILE ChronologTargets.cmake
    NAMESPACE Chronolog::
    DESTINATION ${CHRONOLOG_INSTALL_ROOT}/lib/cmake/Chronolog
)

install(
    FILES ${CMAKE_SOURCE_DIR}/default_client_conf.json.in
    DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR}
    RENAME default_client_conf.json
)
