find_package(mercury REQUIRED)
find_package(thallium REQUIRED)

set(client_examples client_lib_connect_rpc_test client_lib_metadata_rpc_test)

add_custom_target(copyserverlist)

add_custom_command(
        TARGET copyserverlist
        COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/../server_list.in
        ${CMAKE_CURRENT_BINARY_DIR}/server_list)


foreach(client ${client_examples})
    add_executable(${client} ${client}.cpp)
    add_dependencies(${client} copyserverlist)
    target_include_directories(${client} PRIVATE ../Client/include)
    target_link_libraries(${client} ChronoLogClient_lib)
    #add_test(NAME ${client} COMMAND ${client})
endforeach()


