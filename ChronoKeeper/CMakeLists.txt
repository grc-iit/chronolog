cmake_minimum_required( VERSION 3.19)

add_executable(keeper_registry KeeperRegistryInstance.cpp)
target_include_directories(keeper_registry PUBLIC include)
target_link_libraries(keeper_registry mercury thallium)

add_executable(chrono_keeper ChronoKeeperInstance.cpp)
target_include_directories(chrono_keeper PUBLIC include)
target_link_libraries(chrono_keeper mercury thallium)

add_subdirectory(storyteller_client)
