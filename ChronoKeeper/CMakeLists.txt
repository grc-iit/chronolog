cmake_minimum_required( VERSION 3.19)

project(ChronoKeeper)

message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  -Wextra -O0 -g")

add_executable(chrono_keeper)
target_include_directories(chrono_keeper PRIVATE include ../chrono_common ../ChronoAPI/ChronoLog/include)
target_sources(chrono_keeper PRIVATE ChronoKeeperInstance.cpp StoryPipeline.cpp KeeperDataStore.cpp )
target_link_libraries(chrono_keeper thallium)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../default_conf.json.in
        ${CMAKE_CURRENT_BINARY_DIR}/default_conf.json COPYONLY)
message("build target : chrono_keeper")

