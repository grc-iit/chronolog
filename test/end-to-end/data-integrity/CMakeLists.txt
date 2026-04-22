cmake_minimum_required(VERSION 3.25)

find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# End-to-end data-integrity tests (#567).
#
# Three property tests (story_id / event_count / event_order), each invoked
# against two lenses (HDF5 / Replay). The CTest fixture DataIntegrity_Setup
# injects the bundled reference file via chrono-client-admin -f and waits
# for HDF5 chunks to flush. It does NOT deploy the stack — it assumes one
# is already running (the CI pipeline's Deploy ChronoLog step, or a
# developer's own deploy_local.sh --start). DataIntegrity_Cleanup is a
# no-op for the same reason.
#
# All install-tree paths (admin binary, client conf, output dir) resolve at
# *runtime* from $CHRONOLOG_INSTALL_DIR — both the fixture script and the
# test binaries (via data_integrity::fill_install_defaults) read it. The
# output dir defaults to $CHRONOLOG_INSTALL_DIR/output, which is where
# deploy_local.sh points the grapher by default.
#------------------------------------------------------------------------------

set(_di_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_di_reference "${_di_dir}/reference_input.txt")

set(_di_tests
    data_integrity_story_id_test
    data_integrity_event_count_test
    data_integrity_event_order_test
)

foreach(_t ${_di_tests})
    add_executable(${_t}
        ${_t}.cpp
        ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
    )
    target_include_directories(${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/chrono_common/include
        ${CMAKE_SOURCE_DIR}/Client/cpp/include
        ${HDF5_INCLUDE_DIRS}
    )
    target_link_libraries(${_t} PRIVATE
        chrono_common
        ${HDF5_LIBRARIES}
        thallium
    )
endforeach()

#------------------------------------------------------------------------------
# Fixture: setup / cleanup. CHRONOLOG_INSTALL_DIR is intentionally NOT set
# here — the fixture script reads it from the runtime env (with a default).
#------------------------------------------------------------------------------
add_test(NAME DataIntegrity_Setup
    COMMAND ${CMAKE_COMMAND} -E env
        "REFERENCE_INPUT=${_di_reference}"
        bash ${_di_dir}/data_integrity_fixture_setup.sh)
set_tests_properties(DataIntegrity_Setup PROPERTIES FIXTURES_SETUP DataIntegrityFixture TIMEOUT 360)

add_test(NAME DataIntegrity_Cleanup
    COMMAND bash ${_di_dir}/data_integrity_fixture_cleanup.sh)
set_tests_properties(DataIntegrity_Cleanup PROPERTIES FIXTURES_CLEANUP DataIntegrityFixture TIMEOUT 60)

#------------------------------------------------------------------------------
# Per-property × per-lens registration. The test binaries derive admin-bin
# and client-conf from CHRONOLOG_INSTALL_DIR (env, runtime) when not given
# on the CLI.
#------------------------------------------------------------------------------
set(_di_lenses hdf5 replay)
set(_di_lens_labels HDF5 Replay)

set(_di_props
    data_integrity_story_id_test:StoryId
    data_integrity_event_count_test:EventCount
    data_integrity_event_order_test:EventOrder
)

list(LENGTH _di_lenses _di_n)
math(EXPR _di_n "${_di_n} - 1")

foreach(_pair ${_di_props})
    string(REPLACE ":" ";" _pair_list ${_pair})
    list(GET _pair_list 0 _bin)
    list(GET _pair_list 1 _label)
    foreach(_i RANGE ${_di_n})
        list(GET _di_lenses ${_i} _lens)
        list(GET _di_lens_labels ${_i} _lens_label)
        set(_test_name "EndToEnd_DataIntegrity_${_label}_${_lens_label}")
        add_test(NAME ${_test_name}
            COMMAND ${_bin}
                --lens ${_lens}
                --reference ${_di_reference}
                --chronicle chronicle_0_0
                --story story_0_0)
        set_tests_properties(${_test_name} PROPERTIES
            FIXTURES_REQUIRED DataIntegrityFixture
            TIMEOUT 60)
    endforeach()
endforeach()
