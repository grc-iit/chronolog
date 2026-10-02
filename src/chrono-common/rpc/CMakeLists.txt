add_library(chronolog_common_rpc INTERFACE)
target_include_directories(chronolog_common_rpc INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")
target_link_libraries(chronolog_common_rpc INTERFACE gRPC::grpc++)

include(GoogleTest)
add_executable(chronolog_common_rpc_tests visor_channel_test.cpp)
target_link_libraries(chronolog_common_rpc_tests PRIVATE
    chronolog_common_rpc ${CMAKE_DL_LIBS} chronolog_internal_proto chronolog_warnings GTest::gtest GTest::gtest_main)
gtest_discover_tests(chronolog_common_rpc_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
