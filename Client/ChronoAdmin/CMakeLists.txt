
add_executable(ChronoAdmin client_admin.cpp)

target_include_directories(ChronoAdmin PRIVATE ../Client/include)

target_link_libraries(ChronoAdmin ChronoLogClient_lib)
target_link_libraries(ChronoAdmin thallium)

