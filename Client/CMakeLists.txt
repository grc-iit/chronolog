cmake_minimum_required(VERSION 3.25)

find_package(Thallium REQUIRED)
find_package(spdlog REQUIRED)

# C++ ChronoLog Client
add_subdirectory(cpp)

# Python bindings
if(CHRONOLOG_ENABLE_PYTHON_BINDINGS)
    message("Python binding enabled")
    add_subdirectory(python)
endif()

# C++ client test
add_subdirectory(storyteller_test)

# Interactive and scripted test in C++
add_subdirectory(ChronoAdmin)
