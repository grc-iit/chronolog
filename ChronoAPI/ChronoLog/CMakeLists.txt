project(ChronoLog_API)
MESSAGE(STATUS "Chronolog_API: ${ChronoLog_API_SOURCE_DIR}")

add_library(ChronoLog include/ChronoLog.h src/ChronoLog.cpp include/errcode.h)
target_include_directories(ChronoLog PUBLIC include)

#if(BUILD_TESTING)
#    enable_testing()
#    add_subdirectory(test)
#endif()

find_package(mercury REQUIRED)
find_package(thallium REQUIRED)