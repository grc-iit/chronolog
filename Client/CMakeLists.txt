cmake_minimum_required(VERSION 3.25)

add_subdirectory(cpp)

#if(CHRONOLOG_ENABLE_PYTHON_BINDINGS)
message("Python bindings enabled")
add_subdirectory(python)
#endif()

add_subdirectory(examples)