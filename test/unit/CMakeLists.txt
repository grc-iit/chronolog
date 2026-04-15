cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# Unit test subdirectories
#------------------------------------------------------------------------------
add_subdirectory(chrono-common)
add_subdirectory(chrono-keeper)
add_subdirectory(chrono-store)
# chrono-player tests depend on old Thallium architecture — disabled pending rewrite
# add_subdirectory(chrono-player)
add_subdirectory(chronokvs)
