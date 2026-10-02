# Packages that ChronoLog 4.0 never builds against (M11.4). Config-mode lookups
# only reach packages that a vcpkg tree or CMAKE_PREFIX_PATH deliberately provides.
set(CHRONOLOG_FORBIDDEN_PACKAGES Argobots thallium Margo Mochi mercury libfabric ucx rdmacm ibverbs)
foreach(_package IN LISTS CHRONOLOG_FORBIDDEN_PACKAGES)
    find_package(${_package} QUIET CONFIG NO_CMAKE_SYSTEM_PATH NO_SYSTEM_ENVIRONMENT_PATH)
    if(${_package}_FOUND)
        message(FATAL_ERROR "ChronoLog 4.0 forbids ${_package} (${${_package}_DIR}); remove it from CMAKE_PREFIX_PATH and the vcpkg manifest")
    endif()
endforeach()
