cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# chronolog_client unit tests (GTest)
#------------------------------------------------------------------------------

# ClientIdentity pack/unpack
add_executable(chronolog_client_identity_test
    chronolog_client_identity_test.cpp
)

target_include_directories(chronolog_client_identity_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(chronolog_client_identity_test PRIVATE
    GTest::gtest_main
    chronolog_client
)

set_target_properties(chronolog_client_identity_test PROPERTIES OUTPUT_NAME chronolog-test-client-identity)
gtest_discover_tests(chronolog_client_identity_test TEST_PREFIX "Unit_ChronologClient_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_client_identity_test DESTINATION tests)
endif()

# ChronoClock / ClockState (Visor clock exchange offset math)
add_executable(chronolog_client_chrono_clock_test
    chronolog_client_chrono_clock_test.cpp
)

target_include_directories(chronolog_client_chrono_clock_test PRIVATE
    ${CMAKE_SOURCE_DIR}/client/cpp/include
)

target_link_libraries(chronolog_client_chrono_clock_test PRIVATE
    GTest::gtest_main
    chronolog_client
)

set_target_properties(chronolog_client_chrono_clock_test PROPERTIES OUTPUT_NAME chronolog-test-client-chrono-clock)
gtest_discover_tests(chronolog_client_chrono_clock_test TEST_PREFIX "Unit_ChronologClient_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronolog_client_chrono_clock_test DESTINATION tests)
endif()
