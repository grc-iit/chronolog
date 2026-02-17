cmake_minimum_required(VERSION 3.25)

find_package(MPI REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MARGO REQUIRED margo)

#------------------------------------------------------------------------------
# Client Admin CLI Tool
#------------------------------------------------------------------------------
add_executable(client_admin
    client_admin.cpp
)

target_include_directories(client_admin PRIVATE
    ${CMAKE_SOURCE_DIR}/chrono_common/include
    ${CMAKE_SOURCE_DIR}/Client/cpp/include
    ${MARGO_INCLUDE_DIRS}
)

target_link_directories(client_admin PRIVATE
    ${MARGO_LIBRARY_DIRS}
)

target_link_libraries(client_admin PRIVATE
    chronolog_client
    ${MPI_CXX_LIBRARIES}
    ${MARGO_LIBRARIES}
)

set_target_properties(client_admin PROPERTIES
    OUTPUT_NAME chrono-client-admin
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

configure_file(
    ${CMAKE_SOURCE_DIR}/default_conf.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json
    COPYONLY
)

#------------------------------------------------------------------------------
# Install binary
#------------------------------------------------------------------------------
chronolog_install_target(client_admin)

# Client configuration file is installed by Client/cpp to avoid duplication
