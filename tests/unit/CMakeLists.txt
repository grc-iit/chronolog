cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

add_test(NAME Unit_Deploy_ArchiveCleanup
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/deploy_archive_cleanup_test.sh ${CMAKE_SOURCE_DIR})
add_test(NAME Unit_Performance_ArchiveCleanup
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/performance_archive_cleanup_test.sh ${CMAKE_SOURCE_DIR})

#------------------------------------------------------------------------------
# Unit test subdirectories (one component per folder)
#------------------------------------------------------------------------------
add_subdirectory(chrono-common)
add_subdirectory(chrono-grapher)
add_subdirectory(chrono-keeper)
add_subdirectory(chrono-player)
add_subdirectory(chrono-visor)
add_subdirectory(chronokvs)
add_subdirectory(chronolog-client)
add_subdirectory(chronopubsub)
add_subdirectory(chronosql)
