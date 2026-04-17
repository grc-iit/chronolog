cmake_minimum_required(VERSION 3.25)

find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(Thallium REQUIRED)

#------------------------------------------------------------------------------
# End-to-end data-integrity tests (#567).
#
# Three property tests (story_id / event_count / event_order), each invoked
# against three lenses (CSV / HDF5 / Replay). The CTest fixture
# DataIntegrity_Setup boots a local ChronoLog deployment via deploy_local.sh,
# injects the bundled reference file via chrono-client-admin -f, and waits for
# CSV/HDF5 chunks to flush. DataIntegrity_Cleanup tears it down.
#
# All cases require an installed tree pointed to by CHRONOLOG_INSTALL_DIR
# (defaults to $HOME/chronolog-install/chronolog). When the install is
# missing, the setup fixture exits 0 and the dependent cases find no events
# and return SKIP.
#------------------------------------------------------------------------------

set(_di_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_di_csv_out "${CMAKE_CURRENT_BINARY_DIR}/output")
set(_di_h5_out  "$ENV{HOME}/chronolog-install/chronolog/output")
set(_di_install "$ENV{HOME}/chronolog-install/chronolog")
set(_di_reference "${_di_dir}/reference_input.txt")

set(_di_tests
    data_integrity_story_id_test
    data_integrity_event_count_test
    data_integrity_event_order_test
)

foreach(_t ${_di_tests})
    add_executable(${_t}
        ${_t}.cpp
        ${CMAKE_SOURCE_DIR}/ChronoStore/src/StoryReader.cpp
        ${CMAKE_SOURCE_DIR}/ChronoStore/src/StoryWriter.cpp
        ${CMAKE_SOURCE_DIR}/Client/cpp/src/chrono_monitor.cpp
    )
    target_include_directories(${_t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/ChronoStore/include
        ${CMAKE_SOURCE_DIR}/ChronoKeeper/include
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
# Fixture: setup / cleanup
#------------------------------------------------------------------------------
add_test(NAME DataIntegrity_Setup
    COMMAND ${CMAKE_COMMAND} -E env
        "CHRONOLOG_INSTALL_DIR=${_di_install}"
        "REFERENCE_INPUT=${_di_reference}"
        "CSV_OUTPUT_DIR=${_di_csv_out}"
        "HDF5_OUTPUT_DIR=${_di_h5_out}"
        bash ${_di_dir}/data_integrity_fixture_setup.sh)
set_tests_properties(DataIntegrity_Setup PROPERTIES FIXTURES_SETUP DataIntegrityFixture TIMEOUT 180)

add_test(NAME DataIntegrity_Cleanup
    COMMAND ${CMAKE_COMMAND} -E env
        "CHRONOLOG_INSTALL_DIR=${_di_install}"
        bash ${_di_dir}/data_integrity_fixture_cleanup.sh)
set_tests_properties(DataIntegrity_Cleanup PROPERTIES FIXTURES_CLEANUP DataIntegrityFixture TIMEOUT 60)

#------------------------------------------------------------------------------
# Per-property × per-lens registration
#------------------------------------------------------------------------------
set(_di_lenses csv hdf5 replay)
set(_di_lens_labels CSV HDF5 Replay)

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
                --csv-dir ${_di_csv_out}
                --hdf5-dir ${_di_h5_out}
                --chronicle chronicle_0_0
                --story story_0_0
                --admin-bin ${_di_install}/bin/chrono-client-admin
                --client-conf ${_di_install}/conf/chrono-client-conf.json)
        set_tests_properties(${_test_name} PROPERTIES
            FIXTURES_REQUIRED DataIntegrityFixture
            TIMEOUT 60)
    endforeach()
endforeach()
