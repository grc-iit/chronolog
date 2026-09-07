# chrono-ldms runnable example: a self-contained round trip that records
# LDMS-shaped metric samples (as store_chronolog does from an L2 aggregator)
# and reads them back via BOTH the tail read (StoryHandle::playback()) and the
# archive read (Client::ReplayStory()). It is a plain ChronoLog client, so it
# links chronolog_client directly rather than the store_chronolog plugin module
# (which is dlopen()'d by ldmsd, not linked).
add_executable(chrono_ldms_roundtrip_example chrono_ldms_roundtrip_example.cpp)

target_link_libraries(chrono_ldms_roundtrip_example PRIVATE chronolog_client)

# chronolog_client.h / ClientConfiguration.h / cmd_arg_parse.h live in
# chronolog_client's public include dir.
target_include_directories(chrono_ldms_roundtrip_example PRIVATE
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)

set_target_properties(chrono_ldms_roundtrip_example PROPERTIES
    OUTPUT_NAME chrono-ldms-example-roundtrip
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
)

chronolog_install_target(chrono_ldms_roundtrip_example DESTINATION examples)

# Also install the ldmsd configuration example alongside the binary so users
# have a ready-to-adapt "load + configure store_chronolog" recipe.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/ldmsd_store_chronolog.conf
        DESTINATION ${CHRONOLOG_INSTALL_EXAMPLES_DIR})
