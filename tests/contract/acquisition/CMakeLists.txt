add_executable(chronolog_acquisition_contract_tests acquisition_adapter_test.cpp)
target_include_directories(chronolog_acquisition_contract_tests BEFORE PRIVATE
    "${PROJECT_SOURCE_DIR}/src/chrono-visor" "${PROJECT_SOURCE_DIR}/src/chrono-visor/tests")
target_link_libraries(chronolog_acquisition_contract_tests PRIVATE
    chronolog_visor_lib chronolog_keeper_lib chronolog_warnings GTest::gtest GTest::gtest_main)
include(GoogleTest)
if("thread" IN_LIST CHRONOLOG_SANITIZERS AND CHRONOLOG_SETARCH)
    set_property(TARGET chronolog_acquisition_contract_tests PROPERTY CROSSCOMPILING_EMULATOR
        "${CHRONOLOG_SETARCH};${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
endif()
gtest_discover_tests(chronolog_acquisition_contract_tests DISCOVERY_TIMEOUT 60 DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
