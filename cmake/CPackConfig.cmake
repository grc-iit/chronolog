# =============================================================================
# CPack packaging configuration for ChronoLog
# Included by root CMakeLists.txt BEFORE include(CPack).
# All CHRONOLOG_* version variables are already set by the root CMakeLists.txt.
# =============================================================================

# --- General package metadata ------------------------------------------------
set(CPACK_PACKAGE_NAME        "chronolog")
set(CPACK_PACKAGE_VENDOR      "GRC-IIT, Illinois Institute of Technology")
set(CPACK_PACKAGE_CONTACT     "egonzalez30@illinoistech.edu")
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
#   chronolog-2.7.0/bin/, chronolog-2.7.0/lib/, chronolog-2.7.0/include/, ...
#
# Design note — intentional asymmetry between install dir and tarball dir:
#
#   Install tree:  ${CMAKE_INSTALL_PREFIX}/chronolog/
#   Tarball dir:   chronolog-VERSION/
#
#   "chronolog/" is a *stable, version-independent* path.  All operational
#   tooling (CI/CD workflows, deploy scripts, Docker scripts, conf files, and
#   RPATH entries) hard-code this path so they work without knowing the version.
#   Changing it to "chronolog-VERSION/" would require sweeping updates across
#   .github/workflows/ci.yml (20+ references), tools/deploy/ChronoLog/,
#   CI/docker/, and every piece of documentation that quotes WORK_DIR.
#
#   The tarball uses "chronolog-VERSION/" so that users can extract multiple
#   releases side-by-side without collisions.  These two names serve different
#   purposes; the mismatch is deliberate, not an oversight.
#
#   If versioned side-by-side installs are required on a single machine, version
#   the *prefix* instead:
#     cmake --install . --prefix ~/chronolog-install/2.7.0
#   resulting in ~/chronolog-install/2.7.0/chronolog/, and use a symlink
#   ~/chronolog-install/chronolog -> ~/chronolog-install/2.7.0/chronolog
#   to point the operational tooling at the active version.
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
# This file drives the TGZ run only.  RPM and DEB each have their own config
# file (cmake/CPackRPMConfig.cmake.in / CPackDEBConfig.cmake.in) that is
# configured at cmake time and invoked separately by the package_release target.
# Keeping generators separate is required because CPACK_INSTALLED_DIRECTORIES
# cannot be overridden per-generator: TGZ needs "chronolog-VERSION/" as the
# top-level dir while RPM/DEB must place files at "opt/chronolog" (→ /opt/chronolog).
set(CPACK_GENERATOR "TGZ")

# --- Source package ----------------------------------------------------------
# Produce a curated source tarball via `cpack --config CPackSourceConfig.cmake`
# (wrapped by the `package_source` custom target defined in CMakeLists.txt).
# Excludes build artifacts, node_modules, caches, and local tooling directories
# so users get a clean, versioned source package.
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CHRONOLOG_PACKAGE_VERSION}-source")

# --- Source ignore patterns --------------------------------------------------
# Patterns are POSIX regexes matched against absolute paths. Keep these in sync
# with .gitignore where it makes sense — .gitignore covers developer workflow,
# this list protects the release tarball from build/cache bloat.
set(CPACK_SOURCE_IGNORE_FILES
    # VCS / CI metadata
    "/[.]git/"
    "/[.]github/"
    "[.]gitignore$"
    "[.]gitmodules$"
    "[.]gitattributes$"
    "[.]travis[.]yml$"
    # Editor / AI-agent tooling
    "/[.]idea/"
    "/[.]vscode/"
    "/[.]cursor/"
    "/[.]claude/"
    "/[.]aider[^/]*"
    "/[.]windsurf/"
    "/[.]codeium/"
    "/[.]copilot/"
    "/[.]continue/"
    "/[.]mcp/"
    "/[.]agent[s]?/"
    "/[.]local/"
    "/[.]localdev/"
    # Build / CMake artifacts
    "/build/"
    "/chronolog-build/"
    "/cmake-build-[^/]+/"
    "CMakeCache[.]txt$"
    "CMakeFiles/"
    "_CPack_Packages/"
    "[.]spack-env/"
    "spack[.]lock$"
    "compile_commands[.]json$"
    # Node / website build outputs
    "/node_modules/"
    "/[.]docusaurus/"
    "docs-website/build/"
    "website/dist/"
    "/[.]next/"
    "/[.]nuxt/"
    "/[.]turbo/"
    "/[.]parcel-cache/"
    "[.]tsbuildinfo$"
    "[.]eslintcache$"
    # Python / test caches
    "__pycache__/"
    "/[.]pytest_cache/"
    "/[.]mypy_cache/"
    "/[.]ruff_cache/"
    "/[.]cache/"
    "[.]py[cod]$"
    "/venv/"
    "/[.]venv/"
    # Coverage
    "[.]gcno$"
    "[.]gcda$"
    "[.]gcov$"
    "lcov[.]info$"
    "/coverage/"
    "/htmlcov/"
    # OS / editor noise
    "[.]DS_Store$"
    "Thumbs[.]db$"
    "[.]swp$"
    "[.]swo$"
    "~$"
    # Backup / patch files
    "[.]bak$"
    "[.]orig$"
    "[.]rej$"
    "[.]log$"
)

# RPM- and DEB-specific settings live in their own config files
# (cmake/CPackRPMConfig.cmake.in / CPackDEBConfig.cmake.in), configured by
# CMake at build time and invoked separately by the package_release target.