project(ChronoKeeper)

# For new RPC-based implementation
find_package(mercury REQUIRED)
find_package(thallium REQUIRED)
find_package(Threads REQUIRED)

add_library(ChronoKeeper_lib STATIC
        include/Storyteller.h Storyteller.cpp
        KeeperProcess.h KeeperProcess.cpp
	ChronoKeeperInstance.cpp)

target_include_directories(ChronoKeeper_lib PUBLIC include
        ../ChronoAPI/ChronoLog/include)
target_link_libraries(ChronoKeeper_lib thallium)

