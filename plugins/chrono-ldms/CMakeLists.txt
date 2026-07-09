cmake_minimum_required(VERSION 3.25)

project(chrono_ldms
    VERSION 0.1.0
    DESCRIPTION "LDMS ldmsd store plugin backed by ChronoLog"
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# The round-trip example is a pure ChronoLog client (StoryHandle::playback /
# Client::ReplayStory) with no LDMS dependency, so it is ALWAYS built --
# independent of whether the OVIS/LDMS headers required by the plugin below are
# found.
# ---------------------------------------------------------------------------
option(CHRONO_LDMS_BUILD_EXAMPLES "Build chrono-ldms examples" ON)
if(CHRONO_LDMS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# store_chronolog is an ldmsd *store* plugin: a shared object that ldmsd
# dlopen()s at runtime. It needs the OVIS/LDMS development headers
# (ldmsd.h / ldms.h / ldmsd_plug_api.h), which are an external dependency not
# shipped with the ChronoLog superbuild. The plugin is therefore built only
# when those headers are found; otherwise it is quietly skipped so the rest of
# the ChronoLog build is unaffected.
#
# Point the build at an LDMS/OVIS install with either:
#   -DLDMS_PREFIX=/path/to/ldms-install        (CMake cache), or
#   LDMS_PREFIX=/path/to/ldms-install <cmake>  (environment)
# ---------------------------------------------------------------------------
set(LDMS_PREFIX "$ENV{LDMS_PREFIX}" CACHE PATH
    "Path to an LDMS/OVIS install that contains include/ldms/ldmsd.h")

find_path(LDMS_INCLUDE_DIR
    NAMES ldmsd.h
    HINTS ${LDMS_PREFIX}
    PATH_SUFFIXES include/ldms include ldms
)

if(NOT LDMS_INCLUDE_DIR)
    message(STATUS
        "chrono-ldms: LDMS headers (ldmsd.h) not found -- skipping the "
        "store_chronolog plugin (the round-trip example is still built). "
        "Set -DLDMS_PREFIX=<ldms-install> to build the plugin.")
    return()
endif()
message(STATUS "chrono-ldms: building store_chronolog (LDMS headers: ${LDMS_INCLUDE_DIR})")

# ldms.h pulls in sibling headers (zap/zap.h, ovis_util/..., coll/...) from the
# parent include dir, so both <prefix>/include/ldms and <prefix>/include are needed.
get_filename_component(LDMS_INCLUDE_PARENT "${LDMS_INCLUDE_DIR}" DIRECTORY)

# ldmsd loads the plugin by base name 'store_chronolog' -> libstore_chronolog.so.
# MODULE = a dlopen()-only shared object (nothing links against it).
add_library(store_chronolog MODULE
    src/store_chronolog.c
    src/chronolog_bridge.cpp
)

target_include_directories(store_chronolog PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LDMS_INCLUDE_DIR}
    ${LDMS_INCLUDE_PARENT}
    # chronolog_client public headers (chronolog_client.h, ClientConfiguration.h,
    # chronolog_types.h) -- consumed privately by the C++ bridge.
    $<TARGET_PROPERTY:chronolog_client,INTERFACE_INCLUDE_DIRECTORIES>
)

# The ldms_metric_get_* / ldms_set_* symbols are provided by the ldmsd process
# at dlopen time, so we deliberately do NOT link libldms and leave those symbols
# undefined in the plugin. Only the ChronoLog client is linked in.
target_link_libraries(store_chronolog PRIVATE chronolog_client)

# libstore_chronolog.so, unversioned (a dlopen plugin, not a linkable library).
set_target_properties(store_chronolog PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "store_chronolog"
)

# Install the plugin .so into the ChronoLog lib dir. To let ldmsd load it, add
# that directory to LDMSD_PLUGIN_LIBPATH (or copy/symlink the .so into the
# ldmsd plugin directory, e.g. <ldms-install>/lib/ovis-ldms).
chronolog_install_target(store_chronolog)

message(STATUS "chrono-ldms: store_chronolog plugin enabled")
