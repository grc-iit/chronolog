cmake_minimum_required(VERSION 3.25)

find_package(MPI REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MARGO REQUIRED margo)

#------------------------------------------------------------------------------
# Performance benchmark driver
#------------------------------------------------------------------------------
add_executable(perf_bench
    perf_bench.cpp
)

target_include_directories(perf_bench PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${MPI_INCLUDE_PATH}
    ${MARGO_INCLUDE_DIRS}
)

target_link_directories(perf_bench PRIVATE
    ${MARGO_LIBRARY_DIRS}
)

target_link_libraries(perf_bench PRIVATE
    chronolog_client
    ${MPI_CXX_LIBRARIES}
    ${MARGO_LIBRARIES}
)

set_target_properties(perf_bench PROPERTIES
    OUTPUT_NAME chrono-bench
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

chronolog_install_target(perf_bench DESTINATION benchmark)
