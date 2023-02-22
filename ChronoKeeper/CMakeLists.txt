cmake_minimum_required( VERSION 3.19)

project(ChronoKeeper)

message("Building CMAKE_CURRENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(keeper_registry)
#KeeperRegistryInstance.cpp)
target_include_directories(keeper_registry PRIVATE include)
target_sources( keeper_registry PRIVATE KeeperRegistryInstance.cpp )
target_link_libraries(keeper_registry thallium)
message("buidl target : keeper_registry")

add_executable(chrono_keeper)
target_include_directories(chrono_keeper PRIVATE include)
target_sources(chrono_keeper PRIVATE ChronoKeeperInstance.cpp StoryPipeline.cpp KeeperDataStore.cpp )
target_link_libraries(chrono_keeper thallium)
message("build target : chrono_keeper")

add_subdirectory(storyteller_client)
