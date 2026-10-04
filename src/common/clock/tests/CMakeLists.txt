include(GoogleTest)

add_executable(chronolog_common_clock_tests clock_audit_test.cpp kernel_clock_test.cpp visor_clock_audit_test.cpp)
target_link_libraries(chronolog_common_clock_tests PRIVATE
    chronolog_common_clock chronolog_warnings absl::log_sink absl::log_sink_registry GTest::gtest GTest::gtest_main Threads::Threads)
# See the root CMakeLists.txt: ThreadSanitizer needs ASLR disabled on this kernel.
if("thread" IN_LIST CHRONOLOG_SANITIZERS)
    find_program(CHRONOLOG_SETARCH setarch)
    if(CHRONOLOG_SETARCH)
        set_property(TARGET chronolog_common_clock_tests PROPERTY CROSSCOMPILING_EMULATOR
            "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()
endif()
gtest_discover_tests(chronolog_common_clock_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)
