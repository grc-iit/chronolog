project(ChronoLogClient)

set(CMAKE_CXX_STANDARD 17)

#set(CMAKE_FIND_DEBUG_MODE TRUE)

find_package(mercury REQUIRED)
find_package(thallium REQUIRED)

#set(CMAKE_FIND_DEBUG_MODE FALSE)

add_library(ChronoLogClient_lib
        src/client.cpp
        include/client.h
        include/ChronoLogAdminRPCClient.h
        include/ChronicleMetadataRPCClient.h
        ../ChronoAPI/ChronoLog/include/macro.h
        ../ChronoAPI/ChronoLog/include/rpc.h
        ../ChronoAPI/ChronoLog/include/RPCFactory.h
        ../ChronoAPI/ChronoLog/include/singleton.h
        ../ChronoVisor/src/city.cpp
        ../ChronoVisor/include/city.h)
target_include_directories(ChronoLogClient_lib PUBLIC include
        ../ChronoAPI/ChronoLog/include
        ../ChronoVisor/include
        ../Client/include)
target_link_libraries(ChronoLogClient_lib thallium)

#add_executable(ChronoLogClient main.cpp)
#target_link_libraries(ChronoLogClient ChronoLogClient_lib)
#add_dependencies(ChronoLogClient ChronoLogClient_lib)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()