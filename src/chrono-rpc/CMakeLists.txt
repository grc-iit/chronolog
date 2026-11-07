find_package(Threads REQUIRED)
find_package(Thallium REQUIRED)

# Private, engine-independent operation ownership; no archive/HDF5 dependency.
add_library(chrono_rpc SHARED src/RpcRuntime.cpp src/NativeRuntime.cpp)
set_target_properties(chrono_rpc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(chrono_rpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/client/cpp/include)
target_link_libraries(chrono_rpc PRIVATE Threads::Threads thallium)

# Private runtime dependency, not a public SDK target/header surface. One DSO
# holds both process singletons even when client and services load together.
set_target_properties(chrono_rpc PROPERTIES VERSION ${CHRONOLOG_PACKAGE_VERSION}
    SOVERSION ${CHRONOLOG_VERSION_MAJOR} INSTALL_RPATH "${CHRONOLOG_INSTALL_LIB_DIR};$ORIGIN")
install(TARGETS chrono_rpc LIBRARY DESTINATION ${CHRONOLOG_INSTALL_LIB_DIR})
