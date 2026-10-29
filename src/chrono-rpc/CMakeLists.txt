find_package(Threads REQUIRED)
find_package(Thallium REQUIRED)

# Private, engine-independent operation ownership; no archive/HDF5 dependency.
add_library(chrono_rpc OBJECT src/RpcRuntime.cpp)
set_target_properties(chrono_rpc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(chrono_rpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(chrono_rpc PRIVATE Threads::Threads thallium)
