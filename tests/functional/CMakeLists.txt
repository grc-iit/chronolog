cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# Unit test subdirectories (one component per folder)
#------------------------------------------------------------------------------
add_subdirectory(chrono-keeper)
add_subdirectory(chrono-grapher)
add_subdirectory(chrono-player)
