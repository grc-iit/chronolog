
project(clocksync)

cmake_minimum_required(VERSION 3.22)

find_package(MPI REQUIRED)
add_executable(test_clock test_clock.cpp)
target_include_directories(test_clock PRIVATE ${MPI_CXX_INCLUDE_DIRS})
target_link_libraries(test_clock ${MPI_CXX_LIBRARIES} -lm -lrt)
