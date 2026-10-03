cmake_minimum_required(VERSION 3.25)
project(ChronoLogNode LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(chronolog CONFIG REQUIRED)
add_library(chronolog_node SHARED native/addon.cpp native/context.cpp ${CMAKE_JS_SRC})
# The Context library links the shared SDK, so the addon shares that one SDK copy and finds both relative to itself.
get_target_property(context_library chronolog::context LOCATION)
get_filename_component(context_library_dir "${context_library}" DIRECTORY)
file(RELATIVE_PATH context_rpath "${CMAKE_BINARY_DIR}" "${context_library_dir}")
set_target_properties(chronolog_node PROPERTIES PREFIX "" SUFFIX ".node" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    SKIP_BUILD_RPATH ON BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN/${context_rpath}")
target_include_directories(chronolog_node PRIVATE ${CMAKE_JS_INC} "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/node-addon-api")
target_compile_definitions(chronolog_node PRIVATE NAPI_VERSION=9 NAPI_CPP_EXCEPTIONS)
target_compile_options(chronolog_node PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(chronolog_node PRIVATE chronolog::context chronolog::client ${CMAKE_JS_LIB})

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/enum_fixture.cpp")
    add_library(chronolog_enum_test SHARED test/enum_fixture.cpp ${CMAKE_JS_SRC})
    set_target_properties(chronolog_enum_test PROPERTIES PREFIX "" SUFFIX ".node"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_include_directories(chronolog_enum_test PRIVATE ${CMAKE_JS_INC}
        "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/node-addon-api")
    target_compile_definitions(chronolog_enum_test PRIVATE NAPI_VERSION=9 NAPI_CPP_EXCEPTIONS)
    target_compile_options(chronolog_enum_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(chronolog_enum_test PRIVATE chronolog::client ${CMAKE_JS_LIB})
    add_dependencies(chronolog_node chronolog_enum_test)
endif()
