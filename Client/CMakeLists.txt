project(ChronoLogClient)

set(CMAKE_CXX_STANDARD 17)

find_package(thallium REQUIRED)

add_library(chronolog_client
        src/client.cpp
        include/client.h
        include/RPCClient.h
        ../ChronoAPI/ChronoLog/include/macro.h
        ../ChronoAPI/ChronoLog/include/rpc.h
        ../ChronoAPI/ChronoLog/include/RPCFactory.h
        ../ChronoAPI/ChronoLog/include/singleton.h
        ../ChronoAPI/ChronoLog/src/city.cpp
        ../ChronoAPI/ChronoLog/include/city.h)

target_include_directories(chronolog_client PUBLIC include
        ../ChronoAPI/ChronoLog/include
	../chrono_common/
        ../Client/include)

target_link_libraries(chronolog_client thallium)
