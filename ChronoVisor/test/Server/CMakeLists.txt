add_executable(chronovisorserver_test chronovisorserver_test.cpp ../../include/ClientRegistryManager.h ../../src/ClientRegistryManager.cpp)
target_include_directories(chronovisorserver_test PRIVATE ${ChronoVisor_SOURCE_SIR}/include)
target_link_libraries(chronovisorserver_test ChronoVisor_lib)
add_dependencies(chronovisorserver_test ChronoVisor_lib)

add_executable(chronovisormeta_test chroniclemeta_test.cpp ../../include/ClientRegistryManager.h ../../src/ClientRegistryManager.cpp)
target_include_directories(chronovisormeta_test PRIVATE ${ChronoVisor_SOURCE_SIR}/include)
target_link_libraries(chronovisormeta_test ChronoVisor_lib)
add_dependencies(chronovisormeta_test ChronoVisor_lib)

add_test(NAME chronovisorserver_test COMMAND chronovisorserver_test)
add_test(NAME chronovisormeta_test COMMAND chronovisormeta_test)