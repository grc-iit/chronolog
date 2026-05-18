add_executable(high_res_clock_test clock_high_resolution_test.cpp common.h)
add_test(NAME Overhead_Clock_HighResolutionClock COMMAND high_res_clock_test)

add_executable(timestamp_collection clock_timestamp_collection_test.cpp common.h)
add_test(NAME Overhead_Clock_TimestampCollection COMMAND timestamp_collection)

add_executable(timestamp_collision_detection clock_timestamp_collision_test.cpp common.h)
add_test(NAME Overhead_Clock_TimestampCollisionDetection COMMAND timestamp_collision_detection)

# Script-based clock test (run with cset shield; requires root/cgroup, run manually)
add_test(NAME Overhead_Clock_Script
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/clock_cset_shield_test.sh $<TARGET_FILE:high_res_clock_test>)
set_tests_properties(Overhead_Clock_Script PROPERTIES MANUAL TRUE)
set_tests_properties(Overhead_Clock_Script PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})