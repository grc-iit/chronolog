cmake_minimum_required(VERSION 3.25)

find_package(Python COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

message(STATUS "Found pybind11 v${pybind11_VERSION}:${pybind11_INCLUDE_DIRS}")

#------------------------------------------------------------------------------
# ChronoLog Python Client Module
#------------------------------------------------------------------------------
pybind11_add_module(py_chronolog_client MODULE
    py_chronolog_client.cpp
)

add_dependencies(py_chronolog_client
    chronolog_client
)

target_link_libraries(py_chronolog_client
    PRIVATE
        chronolog_client
        pybind11::module
)

#------------------------------------------------------------------------------
# Install Python module
#------------------------------------------------------------------------------
chronolog_install_target(py_chronolog_client)
