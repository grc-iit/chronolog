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
# `set(<var> ... CACHE ...)` only seeds the cache on the FIRST configure of a
# build tree. Because the deploy scripts reuse ~/chronolog-build/<type>, seeding
# it from $ENV{LDMS_PREFIX} alone meant the environment variable was read once
# and silently ignored on every later configure -- so the documented
# `LDMS_PREFIX=... local_single_user_deploy.sh -b` recipe did nothing on an
# existing build tree and the plugin was skipped with no hint why. Declare the
# cache entry empty and consult the environment on every configure, so an
# explicit -DLDMS_PREFIX still wins and the env var keeps working.
set(LDMS_PREFIX "" CACHE PATH
    "Path to an LDMS/OVIS install that contains include/ldms/ldmsd_plug_api.h")
if(NOT LDMS_PREFIX AND DEFINED ENV{LDMS_PREFIX})
    set(LDMS_PREFIX "$ENV{LDMS_PREFIX}")
    message(STATUS "chrono-ldms: using LDMS_PREFIX from the environment: ${LDMS_PREFIX}")
endif()

# Gate on ldmsd_plug_api.h, not ldmsd.h. This plugin targets the NEWER OVIS
# plugin ABI -- ldmsd_plug_handle_t, ldmsd_plug_log_get(), ovis_log(), and a
# `struct ldmsd_store ldmsd_plugin_interface` object. Older OVIS (4.3/4.4) ships
# ldmsd.h but none of that, so gating on ldmsd.h reported the dependency as
# found and then failed the build with a wall of errors from unknown types.
find_path(LDMS_INCLUDE_DIR
    NAMES ldmsd_plug_api.h
    HINTS ${LDMS_PREFIX}
    PATH_SUFFIXES include/ldms include ldms
)

if(NOT LDMS_INCLUDE_DIR)
    # Distinguish "no OVIS at all" from "OVIS too old" -- the second is the
    # confusing one, so name it explicitly.
    find_path(LDMS_LEGACY_INCLUDE_DIR
        NAMES ldmsd.h
        HINTS ${LDMS_PREFIX}
        PATH_SUFFIXES include/ldms include ldms
    )
    if(LDMS_LEGACY_INCLUDE_DIR)
        message(STATUS
            "chrono-ldms: found ldmsd.h in ${LDMS_LEGACY_INCLUDE_DIR} but no ldmsd_plug_api.h -- "
            "this OVIS/LDMS install predates the plugin ABI store_chronolog targets "
            "(ldmsd_plug_handle_t / ovis_log / ldmsd_plugin_interface). Skipping the "
            "store_chronolog plugin; the round-trip example is still built. Upgrade OVIS or "
            "point -DLDMS_PREFIX at a newer install to build it.")
    else()
        message(STATUS
            "chrono-ldms: LDMS headers (ldmsd_plug_api.h) not found -- skipping the "
            "store_chronolog plugin (the round-trip example is still built). "
            "Set -DLDMS_PREFIX=<ldms-install> to build the plugin.")
    endif()
    return()
endif()
message(STATUS "chrono-ldms: building store_chronolog (LDMS headers: ${LDMS_INCLUDE_DIR})")

# ldms.h pulls in sibling headers (zap/zap.h, ovis_util/..., coll/...) from the
# parent include dir, so both <prefix>/include/ldms and <prefix>/include are needed.
get_filename_component(LDMS_INCLUDE_PARENT "${LDMS_INCLUDE_DIR}" DIRECTORY)

# The OVIS headers pull in third-party headers of their own: ldms.h includes
# <openssl/sha.h> and ldmsd.h includes <jansson.h>. OVIS located those through
# its own configure; nothing carries them onto this target, and the plugin calls
# neither library -- it needs the headers only to satisfy the OVIS ones, so the
# include path is required but no link is.
#
# On a host with the distro -devel packages installed both resolve silently from
# /usr/include and the omission is invisible. It is not invisible where OpenSSL
# comes from somewhere else, which is exactly ChronoLog's own situation: the
# spack environment supplies openssl (already pinned in spack.yaml as a cmake
# dependency, so this adds nothing to install), /usr/include has no openssl at
# all, and the build fails in ldms.h with "openssl/sha.h: No such file or
# directory".
#
# Not REQUIRED: a missing OpenSSL skips this optional plugin the same way a
# missing or too-old OVIS does, rather than failing the whole ChronoLog
# configure over it.
#
# jansson is not searched for here because it is installed alongside OVIS often
# enough that LDMS_INCLUDE_PARENT covers it. If a build fails on <jansson.h>,
# add its include dir the same way.
find_package(OpenSSL)
if(NOT OPENSSL_FOUND)
    message(STATUS
        "chrono-ldms: OpenSSL headers not found -- skipping the store_chronolog "
        "plugin (the round-trip example is still built). ldms.h includes "
        "<openssl/sha.h>, so the plugin cannot compile without them.")
    return()
endif()

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
    # Headers only -- see the find_package(OpenSSL) note above.
    ${OPENSSL_INCLUDE_DIR}
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
