cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

#message("ChronologClient CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ChronologClient CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : chronolog_client")


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
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/src/StoryChunk.cpp
)

# Include directories for the library
target_include_directories(chronolog_client PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/include
)

target_link_libraries(chronolog_client thallium)

################################

# Define the header files to be copied
set(PUBLIC_HEADER_FILES
    include/chronolog_client.h
    include/ClientConfiguration.h
    include/client_errcode.h
)

# install library
install(
    TARGETS chronolog_client DESTINATION lib
)

# Install header files
install(
    FILES ${PUBLIC_HEADER_FILES} DESTINATION include 
)

# install client configuration file
install(
    FILES ${CMAKE_SOURCE_DIR}/default_client_conf.json.in
    DESTINATION ${CHRONOLOG_INSTALL_CONF_DIR}
    RENAME default_client_conf.json
)

add_subdirectory(ChronoAdmin)
add_subdirectory(examples)