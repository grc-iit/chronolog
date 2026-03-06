# =============================================================================
# CPack packaging configuration for ChronoLog
# Included by root CMakeLists.txt BEFORE include(CPack).
# All CHRONOLOG_* version variables are already set by the root CMakeLists.txt.
# =============================================================================

# --- General package metadata ------------------------------------------------
set(CPACK_PACKAGE_NAME        "chronolog")
set(CPACK_PACKAGE_VENDOR      "GRC-IIT, Illinois Institute of Technology")
set(CPACK_PACKAGE_CONTACT     "eneko.gonzalez@iit.edu")
set(CPACK_PACKAGE_VERSION     "${CHRONOLOG_PACKAGE_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "ChronoLog: Distributed Chronicle Logging System")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README  "${CMAKE_SOURCE_DIR}/README.md")

# Disable CPack's own cmake --install step; package the tree already prepared
# by install.sh (which runs cmake --install + copy_shared_libs + update_rpath).
# Prerequisite: run install.sh -i before invoking cpack.
#
# CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF: CPack will NOT wrap content in an extra
# top-level directory, so the destination below becomes the only level:
#   chronolog-2.5.0/bin/, chronolog-2.5.0/lib/, chronolog-2.5.0/include/, ...
set(CPACK_INSTALL_CMAKE_PROJECTS "")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_INSTALLED_DIRECTORIES
    "${CMAKE_INSTALL_PREFIX}/chronolog"              # fully prepared tree (libs bundled, RPATH patched)
    "${CPACK_PACKAGE_NAME}-${CHRONOLOG_PACKAGE_VERSION}"   # top-level dir inside the tarball
)

# CPACK_PACKAGE_FILE_NAME controls the tarball filename.
# The platform suffix (linux-x86_64) is added by the package_release custom
# target defined in CMakeLists.txt after include(CPack).
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CHRONOLOG_PACKAGE_VERSION}")

# --- Generators --------------------------------------------------------------
# Always produce a TGZ. Add RPM when rpmbuild is present on this host.
set(CPACK_GENERATOR "TGZ")

find_program(RPMBUILD_FOUND rpmbuild)
if(RPMBUILD_FOUND)
    list(APPEND CPACK_GENERATOR "RPM")
    message(STATUS "CPack: rpmbuild found — RPM generator enabled")
endif()

find_program(DPKG_FOUND dpkg-deb)
if(DPKG_FOUND)
    list(APPEND CPACK_GENERATOR "DEB")
    message(STATUS "CPack: dpkg-deb found — DEB generator enabled")
endif()

# --- Source ignore patterns --------------------------------------------------
set(CPACK_SOURCE_IGNORE_FILES
    "/[.]git/"
    "/[.]github/"
    "/build/"
    "/chronolog-build/"
    "[.]gitignore$"
    "[.]gitmodules$"
    "[.]clang-format$"
    "CMakeCache[.]txt$"
    "CMakeFiles/"
    "_CPack_Packages/"
)

# --- RPM-specific settings ---------------------------------------------------
set(CPACK_RPM_PACKAGE_LICENSE  "BSD-2-Clause")
set(CPACK_RPM_PACKAGE_GROUP    "System Environment/Daemons")
set(CPACK_RPM_PACKAGE_REQUIRES "json-c >= 0.13")
set(CPACK_RPM_PACKAGE_DESCRIPTION
"ChronoLog is a distributed, hierarchical, and tiered chronicle-based logging
system designed for high-performance recording and playback of time-ordered
event streams across HPC clusters.

Components: ChronoVisor, ChronoKeeper, ChronoGrapher, ChronoPlayer, chronolog_client.")

# --- DEB-specific settings ---------------------------------------------------
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libjson-c5 (>= 0.13)")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
