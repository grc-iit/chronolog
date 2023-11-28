find_package(MPI REQUIRED)

add_executable(client_admin client_admin.cpp)

target_link_libraries(client_admin chronolog_client thallium ${MPI_CXX_LIBRARIES})

configure_file(${CMAKE_SOURCE_DIR}/default_conf.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)

set_target_properties(client_admin PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

# install binary
install(
    TARGETS client_admin DESTINATION bin
)

# install configuration file
install(
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/../../default_conf.json.in DESTINATION conf RENAME default_conf.json
)