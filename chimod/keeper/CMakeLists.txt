cmake_minimum_required(VERSION 3.10)

add_chimod_client(
  SOURCES
    src/keeper_client.cc
  INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

add_chimod_runtime(
  SOURCES
    src/keeper_runtime.cc
    src/autogen/keeper_lib_exec.cc
  INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

# Link chrono_common as PRIVATE to avoid export-set issues
# (the chimod export system only tracks PUBLIC deps)
target_link_libraries(chronolog_keeper_runtime PRIVATE chrono_common)
