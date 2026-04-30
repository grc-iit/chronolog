cmake_minimum_required(VERSION 3.25)

find_package(GTest REQUIRED)
include(GoogleTest)

#------------------------------------------------------------------------------
# ChronoSQL parser/codec/metadata unit tests
# These do not require a live ChronoLog deployment.
#------------------------------------------------------------------------------
add_executable(chronosql_unit_test
    chronosql_sql_parser_test.cpp
    chronosql_row_codec_test.cpp
    chronosql_metadata_test.cpp
)

target_include_directories(chronosql_unit_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronosql/include
    ${CMAKE_SOURCE_DIR}/Plugins/chronosql/src
)

target_link_libraries(chronosql_unit_test PRIVATE
    chronosql
    GTest::gtest_main
)

set_target_properties(chronosql_unit_test PROPERTIES OUTPUT_NAME chronolog-test-chronosql-unit)
gtest_discover_tests(chronosql_unit_test TEST_PREFIX "Unit_ChronoSQL_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronosql_unit_test DESTINATION tests)
endif()

#------------------------------------------------------------------------------
# ChronoSQL configuration constructor unit test
# Verifies the config_path constructor throws on missing/invalid files before
# attempting to connect.
#------------------------------------------------------------------------------
add_executable(chronosql_config_test
    chronosql_config_test.cpp
)

target_include_directories(chronosql_config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/Plugins/chronosql/include
)

target_link_libraries(chronosql_config_test PRIVATE
    chronosql
    GTest::gtest_main
)

set_target_properties(chronosql_config_test PROPERTIES OUTPUT_NAME chronolog-test-chronosql-config)
gtest_discover_tests(chronosql_config_test TEST_PREFIX "Unit_ChronoSQL_")
if(CHRONOLOG_INSTALL_TESTS)
    chronolog_install_target(chronosql_config_test DESTINATION tests)
endif()
