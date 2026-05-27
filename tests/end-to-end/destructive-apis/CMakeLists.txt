cmake_minimum_required(VERSION 3.25)

#------------------------------------------------------------------------------
# End-to-end destructive-API tests (issue #574).
#
# Each test drives a running ChronoLog stack via chronolog::Client and then
# checks the Grapher's HDF5 archive directory to verify destructive
# operations (DestroyStory, DestroyChronicle, Disconnect auto-release) are
# actually destructive.
#
# Tests do NOT deploy a stack — they assume one is already running (CI's
# Deploy step, or a developer's deploy_local.sh --start). They locate the
# stack's output directory via $CHRONOLOG_INSTALL_DIR at runtime, falling
# back to $HOME/chronolog-install/chronolog (the deploy_local.sh default).
#------------------------------------------------------------------------------

set(_da_dir "${CMAKE_CURRENT_SOURCE_DIR}")

set(_da_props
    destructive_apis_destroy_story_test:DestroyStory
    destructive_apis_destroy_chronicle_test:DestroyChronicle
    destructive_apis_destroy_refused_test:DestroyRefused
    destructive_apis_disconnect_autorelease_test:DisconnectAutoRelease
)

foreach(_pair ${_da_props})
    string(REPLACE ":" ";" _pair_list ${_pair})
    list(GET _pair_list 0 _bin)
    add_executable(${_bin}
        ${_bin}.cpp
    )
    target_include_directories(${_bin} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/client/cpp/include
    )
    target_link_libraries(${_bin} PRIVATE
        chronolog_client
    )
endforeach()

add_test(NAME DestructiveAPIs_Setup
    COMMAND bash ${_da_dir}/destructive_apis_fixture_setup.sh)
set_tests_properties(DestructiveAPIs_Setup PROPERTIES FIXTURES_SETUP DestructiveAPIsFixture TIMEOUT 60)

add_test(NAME DestructiveAPIs_Cleanup
    COMMAND bash ${_da_dir}/destructive_apis_fixture_cleanup.sh)
set_tests_properties(DestructiveAPIs_Cleanup PROPERTIES FIXTURES_CLEANUP DestructiveAPIsFixture TIMEOUT 60)

foreach(_pair ${_da_props})
    string(REPLACE ":" ";" _pair_list ${_pair})
    list(GET _pair_list 0 _bin)
    list(GET _pair_list 1 _label)
    set(_test_name "EndToEnd_DestructiveAPIs_${_label}")
    add_test(NAME ${_test_name}
        COMMAND ${_bin}
            --client-conf $ENV{HOME}/chronolog-install/chronolog/conf/default-chrono-client-conf.json)
    set_tests_properties(${_test_name} PROPERTIES
        FIXTURES_REQUIRED DestructiveAPIsFixture
        # Tests poll for the Grapher's async persistence / destroy worker to
        # catch up; give a busy runner some margin around the 30 s poll cap
        # in the test helpers.
        TIMEOUT 120)
endforeach()
