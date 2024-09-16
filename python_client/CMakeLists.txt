cmake_minimum_required(VERSION 3.25)

###################################
# Find Packages
###################################
find_package(Python COMPONENTS Interpreter Development REQUIRED)
find_package(pybind11 CONFIG REQUIRED)
message(STATUS "Found pybind11 v${pybind11_VERSION}:${pybind11_INCLUDE_DIRS}")

####################################
# Compile ChronoLog Python Client
####################################

pybind11_add_module(py_chronolog_client MODULE py_chronolog_client.cpp)

add_dependencies(py_chronolog_client 
            #${chronolog_CLIENT_DEPS} 
            chronolog_client)
target_link_libraries(py_chronolog_client PUBLIC 
                #${chronolog_CLIENT_LIBRARIES} 
                chronolog_client pybind11::module)

###################################
# Install targets
##################################
install(TARGETS py_chronolog_client DESTINATION lib)
