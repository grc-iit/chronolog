cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# Unit test subdirectories (one component per folder)
#------------------------------------------------------------------------------
add_subdirectory(chrono-common)
add_subdirectory(chrono-store)
add_subdirectory(chrono-player)
add_subdirectory(chronokvs)
add_subdirectory(chronopubsub)
