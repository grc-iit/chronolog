add_library(chronolog_common_worker STATIC WorkerPool.cpp)
target_include_directories(chronolog_common_worker PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")
target_compile_features(chronolog_common_worker PUBLIC cxx_std_20)
target_link_libraries(chronolog_common_worker PUBLIC Threads::Threads PRIVATE chronolog_warnings)

include(GoogleTest)
add_executable(chronolog_common_worker_tests worker_pool_test.cpp)
target_link_libraries(chronolog_common_worker_tests PRIVATE
    chronolog_common_worker chronolog_warnings GTest::gtest GTest::gtest_main)
# See the root CMakeLists.txt: ThreadSanitizer needs ASLR disabled on this kernel.
if("thread" IN_LIST CHRONOLOG_SANITIZERS)
    find_program(CHRONOLOG_SETARCH setarch)
    if(CHRONOLOG_SETARCH)
        set_property(TARGET chronolog_common_worker_tests PROPERTY CROSSCOMPILING_EMULATOR
            "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()
endif()
gtest_discover_tests(chronolog_common_worker_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES
    PROPERTIES TIMEOUT 30)
