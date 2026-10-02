include(GoogleTest)

add_executable(chronolog_keeper_tests
    acquisition_watcher_test.cpp
    cluster_client_test.cpp
    config_test.cpp
    fetch_hot_adapter_test.cpp
    hlc_core_test.cpp
    journal_adapter_test.cpp
    "${CMAKE_SOURCE_DIR}/tests/contract/journal/journal_adapter_test.cpp"
    "${CMAKE_SOURCE_DIR}/tests/contract/archive/keeper_archive_adapter_test.cpp"
    ram_journal_contract.cpp
    ram_journal_test.cpp
    system_clock_contract.cpp)
target_include_directories(chronolog_keeper_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/tests/contract")
target_link_libraries(chronolog_keeper_tests PRIVATE
    chronolog_keeper_lib chronolog_keeper_journal chronolog_common_clock chronolog_warnings
    GTest::gtest GTest::gtest_main Threads::Threads)
# See the root CMakeLists.txt: ThreadSanitizer needs ASLR disabled on this kernel.
if("thread" IN_LIST CHRONOLOG_SANITIZERS)
    find_program(CHRONOLOG_SETARCH setarch)
    if(CHRONOLOG_SETARCH)
        set_property(TARGET chronolog_keeper_tests PROPERTY CROSSCOMPILING_EMULATOR
            "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
    endif()
endif()
gtest_discover_tests(chronolog_keeper_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST NO_PRETTY_VALUES PROPERTIES TIMEOUT 30)
