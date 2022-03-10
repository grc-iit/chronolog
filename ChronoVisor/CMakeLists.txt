project(Chronovisor)

add_library(ChronoVisor_lib STATIC
        include/ChronoVisorServer.h include/ClientRegistryRecord.h
        include/SerVector.h include/TimeRecord.h
        src/ChronoVisorServer.cpp src/ClientRegistryRecord.cpp
        src/TimeRecord.cpp
        src/TimeManager.cpp include/TimeManager.h
        src/ClocksourceManager.cpp include/ClocksourceManager.h
        src/ChronicleMetaDirectory.cpp include/ChronicleMetaDirectory.h
        include/Chronicle.h include/Story.h include/Archive.h include/Event.h
        src/city.cpp include/city.h include/config.h)
target_include_directories(ChronoVisor_lib PUBLIC include ${ROCKSDB_INCLUDE}) #-I/usr/include
target_link_libraries(ChronoVisor_lib socketpp Serde ${ROCKSDB_LIB} pthread) #_L/usr/bin/mpi.so
add_dependencies(ChronoVisor_lib socketpp Serde)

add_executable(ChronoVisor main.cpp)
target_link_libraries(ChronoVisor ChronoVisor_lib socketpp)
add_dependencies(ChronoVisor ChronoVisor_lib socketpp)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()