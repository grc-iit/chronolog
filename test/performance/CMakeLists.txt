cmake_minimum_required(VERSION 3.25)

find_package(MPI REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MARGO REQUIRED margo)

#------------------------------------------------------------------------------
# Performance / regression test driver
#------------------------------------------------------------------------------
# Extracted from tools/cli/client_admin.cpp (scripted workload). The interactive
# shell counterpart lives at tools/cli/client_cli.cpp.
add_executable(performance_test
    performance_test.cpp
)

target_include_directories(performance_test PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${MPI_INCLUDE_PATH}
    ${MARGO_INCLUDE_DIRS}
)

target_link_directories(performance_test PRIVATE
    ${MARGO_LIBRARY_DIRS}
)

target_link_libraries(performance_test PRIVATE
    chronolog_client
    ${MPI_CXX_LIBRARIES}
    ${MARGO_LIBRARIES}
)

set_target_properties(performance_test PROPERTIES
    OUTPUT_NAME chrono-performance-test
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

chronolog_install_target(performance_test DESTINATION tests)
