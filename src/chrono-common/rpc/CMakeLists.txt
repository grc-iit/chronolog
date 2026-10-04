add_library(chronolog_common_rpc INTERFACE)
target_include_directories(chronolog_common_rpc INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")
target_link_libraries(chronolog_common_rpc INTERFACE gRPC::grpc++ chronolog_contracts)

include(GoogleTest)
add_executable(chronolog_common_rpc_tests channel_pool_test.cpp)
target_link_libraries(chronolog_common_rpc_tests PRIVATE
    chronolog_common_rpc ${CMAKE_DL_LIBS} chronolog_internal_proto chronolog_warnings GTest::gtest GTest::gtest_main)
gtest_discover_tests(chronolog_common_rpc_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)

add_library(chronolog_common_visor_clock_audit STATIC VisorClockAudit.cpp)
target_link_libraries(chronolog_common_visor_clock_audit
    PUBLIC chronolog_common_rpc chronolog_common_clock chronolog_internal_proto PRIVATE chronolog_warnings absl::log)

add_executable(chronolog_common_visor_clock_audit_tests visor_clock_audit_test.cpp)
target_link_libraries(chronolog_common_visor_clock_audit_tests PRIVATE chronolog_common_visor_clock_audit
    chronolog_warnings absl::log_sink absl::log_sink_registry GTest::gtest GTest::gtest_main)
# See the root CMakeLists.txt: ThreadSanitizer needs ASLR disabled on this kernel.
if("thread" IN_LIST CHRONOLOG_SANITIZERS)
    find_program(CHRONOLOG_SETARCH setarch)
    if(CHRONOLOG_SETARCH)
        set_property(TARGET chronolog_common_visor_clock_audit_tests PROPERTY CROSSCOMPILING_EMULATOR
            "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()
endif()
gtest_discover_tests(chronolog_common_visor_clock_audit_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST
    NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)
