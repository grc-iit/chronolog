cmake_minimum_required(VERSION 3.25)

find_package(HDF5 REQUIRED COMPONENTS C CXX)

#------------------------------------------------------------------------------
# End-to-end data-integrity tests (#567).
#
# Three property tests (StoryId / EventCount / EventOrder) read ChronoGrapher's
# on-disk HDF5 archive and verify it matches the reference input that
# DataIntegrity_Setup injected via chrono-client-admin -f.
#
# The fixture does NOT deploy the stack — it assumes one is already running
# (the CI pipeline's Deploy ChronoLog step, or a developer's own
# deploy_local.sh --start). DataIntegrity_Cleanup is a no-op for the same
# reason.
#
# The output dir (for both the fixture's wait and the tests' scan) defaults
# to $CHRONOLOG_INSTALL_DIR/output at *runtime*, with a fallback to
# $HOME/chronolog-install/chronolog/output — matching the deploy_local.sh
# default and the fixture script. That avoids baking configure-time paths
# (which may reflect a different user / HOME) into the test binaries.
#------------------------------------------------------------------------------

set(_di_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_di_reference "${_di_dir}/reference_input.txt")

set(_di_props
    data_integrity_story_id_test:StoryId
    data_integrity_event_count_test:EventCount
    data_integrity_event_order_test:EventOrder
)

foreach(_pair ${_di_props})
    string(REPLACE ":" ";" _pair_list ${_pair})
    list(GET _pair_list 0 _bin)
    add_executable(${_bin}
        ${_bin}.cpp
    )
    target_include_directories(${_bin} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/chrono_common/include
        ${HDF5_INCLUDE_DIRS}
    )
    target_link_libraries(${_bin} PRIVATE
        chrono_common
        ${HDF5_LIBRARIES}
    )
endforeach()

add_test(NAME DataIntegrity_Setup
    COMMAND ${CMAKE_COMMAND} -E env
        "REFERENCE_INPUT=${_di_reference}"
        bash ${_di_dir}/data_integrity_fixture_setup.sh)
set_tests_properties(DataIntegrity_Setup PROPERTIES FIXTURES_SETUP DataIntegrityFixture TIMEOUT 360)

add_test(NAME DataIntegrity_Cleanup
    COMMAND bash ${_di_dir}/data_integrity_fixture_cleanup.sh)
set_tests_properties(DataIntegrity_Cleanup PROPERTIES FIXTURES_CLEANUP DataIntegrityFixture TIMEOUT 60)

foreach(_pair ${_di_props})
    string(REPLACE ":" ";" _pair_list ${_pair})
    list(GET _pair_list 0 _bin)
    list(GET _pair_list 1 _label)
    set(_test_name "EndToEnd_DataIntegrity_${_label}")
    add_test(NAME ${_test_name}
        COMMAND ${_bin}
            --reference ${_di_reference}
            --chronicle chronicle_0_0
            --story story_0_0)
    set_tests_properties(${_test_name} PROPERTIES
        FIXTURES_REQUIRED DataIntegrityFixture
        TIMEOUT 60)
endforeach()
