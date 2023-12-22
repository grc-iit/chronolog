project(ChronoLogClient)

set(CMAKE_CXX_STANDARD 17)

find_package(thallium REQUIRED)

# Define the header files to be copied
set(HEADER_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/chronolog_client.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../chrono_common/ConfigurationManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../ChronoAPI/ChronoLog/include/chronolog_errcode.h
)

# Set the destination directory for the header files
set(HEADER_DEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../build/Client/include)

# Create the destination directory
file(MAKE_DIRECTORY ${HEADER_DEST_DIR})

# Copy header files to the include directory during build
foreach(HEADER ${HEADER_FILES})
    configure_file(${HEADER} ${HEADER_DEST_DIR} COPYONLY)
endforeach()

add_library(chronolog_client
    include/chronolog_client.h
    src/ChronologClientImpl.h
    src/KeeperRecordingClient.h
    src/StorytellerClient.h
    src/StorytellerClient.cpp
    src/ChronologClient.cpp
    src/ChronologClientImpl.cpp
    ../ChronoAPI/ChronoLog/src/log.cpp
)

# Include directories for the library
target_include_directories(chronolog_client PUBLIC include
    ../ChronoAPI/ChronoLog/include
    ../chrono_common/
    ../Client/include
)

target_link_libraries(chronolog_client thallium)

add_subdirectory(storyteller_test)

# install library
install(
    TARGETS chronolog_client DESTINATION lib
)

# Install header files
install(
    FILES ${HEADER_FILES}
    DESTINATION ${HEADER_DEST_DIR}
)

#include/client.h
#include/RPCClient.h
#../ChronoAPI/ChronoLog/include/macro.h
#../ChronoAPI/ChronoLog/include/rpc.h
#../ChronoAPI/ChronoLog/include/RPCFactory.h
#../ChronoAPI/ChronoLog/include/singleton.h
#../ChronoAPI/ChronoLog/src/city.cpp
#../ChronoAPI/ChronoLog/include/city.h)