cmake_minimum_required(VERSION 3.25)

message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")
message("build target : client_query_service_test")

add_executable(client_query_service_test 
    ${CMAKE_CURRENT_SOURCE_DIR}/ClientQueryServiceTest.cpp
    ${CMAKE_SOURCE_DIR}/Client/src/ClientQueryService.cpp
    ${CMAKE_SOURCE_DIR}/Client/src/PlaybackQueryRpcClient.cpp
    ${CMAKE_SOURCE_DIR}/chrono_common/StoryChunk.cpp
    )


target_include_directories(client_query_service_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Client/src
    ${CMAKE_SOURCE_DIR}/chrono_common
    ${CMAKE_SOURCE_DIR}/ChronoAPI/ChronoLog/include
)

target_link_libraries(client_query_service_test chronolog_client thallium)

set_target_properties(client_query_service_test PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# install binary
install(
    TARGETS client_admin DESTINATION bin
)

