cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

#message("ChronologClient CMakeLists - CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}")
#message("ChronologClient CMakeLists - CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : chronolog_client")


add_library(chronolog_client
    ${CMAKE_CURRENT_SOURCE_DIR}/include/chronolog_client.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ChronologClientImpl.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/KeeperRecordingClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StorytellerClient.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StorytellerClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PlaybackQueryRpcClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ClientQueryService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ChronologClient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ChronologClientImpl.cpp
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/src/chrono_monitor.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/ConfigurationManager.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp
)

# Include directories for the library
target_include_directories(chronolog_client PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/Client/include
)

target_link_libraries(chronolog_client thallium)

################################

# Define the header files to be copied
set(PUBLIC_HEADER_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/chronolog_client.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ClientConfiguration.h
    ${CMAKE_SOURCE_DIR}/chrono_common/ConfigurationManager.h
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include/chronolog_errcode.h
)

# install library
install(
    TARGETS chronolog_client DESTINATION lib
)

# Install header files
install(
    FILES ${PUBLIC_HEADER_FILES} DESTINATION include 
)

add_subdirectory(storyteller_test)
add_subdirectory(ChronoAdmin)
