
add_executable(ChronoAdmin client_admin.cpp)
add_executable(ChronoAdmin_Sim client_admin_sim.cpp)

target_include_directories(ChronoAdmin PRIVATE ../Client/include)
target_include_directories(ChronoAdmin_Sim PRIVATE ../Client/include)

target_link_libraries(ChronoAdmin ChronoLogClient_lib)
target_link_libraries(ChronoAdmin thallium)
target_link_libraries(ChronoAdmin_Sim ChronoLogClient_lib)
target_link_libraries(ChronoAdmin_Sim thallium)

