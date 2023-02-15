cmake_minimum_required( VERSION 3.19)

add_executable(keeper_registry KeeperRegistryInstance.cpp)
target_include_directories(keeper_registry PUBLIC include)
target_link_libraries(keeper_registry thallium)

add_executable(chrono_keeper ChronoKeeperInstance.cpp)
target_include_directories(chrono_keeper PRIVATE include)
target_sources(chrono_keeper PRIVATE KeeperDataStore.cpp)

target_link_libraries(chrono_keeper thallium)

add_subdirectory(storyteller_client)
