add_executable(chronolog_stub_server stub_server.cpp)
target_link_libraries(chronolog_stub_server PRIVATE
    chronolog_proto chronolog_warnings Threads::Threads)
install(TARGETS chronolog_stub_server RUNTIME DESTINATION bin)
