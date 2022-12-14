project(ChronoLogClient)

set(CMAKE_CXX_STANDARD 17)

#set(CMAKE_FIND_DEBUG_MODE TRUE)

find_package(mercury REQUIRED)
find_package(thallium REQUIRED)

#set(CMAKE_FIND_DEBUG_MODE FALSE)

add_library(ChronoLogClient_lib STATIC
        include/client.h src/client.cpp
        ../ChronoAPI/ChronoLog/include/ChronoLogAdminRPCVisor.h
        ../ChronoAPI/ChronoLog/include/ChronicleMetadataRPCVisor.h
        ../ChronoAPI/ChronoLog/include/macro.h
        ../ChronoAPI/ChronoLog/include/rpc.h
        ../ChronoAPI/ChronoLog/include/RPCFactory.h
        ../ChronoAPI/ChronoLog/include/singleton.h
        ../ChronoVisor/include/ClientRegistryInfo.h
        ../ChronoVisor/include/ClientRegistryManager.h
        ../ChronoVisor/src/ClientRegistryRecord.cpp
        ../ChronoVisor/src/ClientRegistryManager.cpp
        ../ChronoVisor/src/ChronicleMetaDirectory.cpp
        ../ChronoVisor/include/Chronicle.h
        ../ChronoVisor/include/Story.h
        ../ChronoVisor/include/Event.h
        ../ChronoVisor/src/city.cpp
        ../ChronoVisor/include/city.h)
target_include_directories(ChronoLogClient_lib PUBLIC include
        ../ChronoAPI/ChronoLog/include
        ../ChronoVisor/include)
target_link_libraries(ChronoLogClient_lib thallium)

#add_executable(ChronoLogClient main.cpp)
#target_link_libraries(ChronoLogClient ChronoLogClient_lib)
#add_dependencies(ChronoLogClient ChronoLogClient_lib)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()