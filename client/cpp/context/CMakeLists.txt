find_package(OpenSSL REQUIRED)
add_library(chronolog_context SHARED context.cpp recall.cpp follow.cpp)
add_library(chronolog::context ALIAS chronolog_context)
target_compile_features(chronolog_context PUBLIC cxx_std_20)
target_include_directories(chronolog_context PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(chronolog_context PUBLIC chronolog::client PRIVATE chronolog_warnings
    nlohmann_json::nlohmann_json OpenSSL::Crypto Threads::Threads)
set_target_properties(chronolog_context PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 4 EXPORT_NAME context)
install(TARGETS chronolog_context EXPORT chronologClientTargets LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT client)
install(DIRECTORY include/chronolog/context DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/chronolog COMPONENT client)

add_executable(chronolog_context_tests context_test.cpp)
target_link_libraries(chronolog_context_tests PRIVATE chronolog::context chronolog_proto chronolog_warnings
    GTest::gtest_main Threads::Threads)
gtest_discover_tests(chronolog_context_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST
    TEST_PREFIX "context." PROPERTIES TIMEOUT 30)
