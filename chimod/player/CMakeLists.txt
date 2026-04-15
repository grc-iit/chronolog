cmake_minimum_required(VERSION 3.10)

add_chimod_client(
  SOURCES
    src/player_client.cc
  INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

add_chimod_runtime(
  SOURCES
    src/player_runtime.cc
    src/autogen/player_lib_exec.cc
  INCLUDE_DIRECTORIES
    ${CMAKE_SOURCE_DIR}/chrono_common/include
)

target_link_libraries(chronolog_player_runtime PRIVATE chrono_common)
