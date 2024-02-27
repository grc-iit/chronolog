cmake_minimum_required(VERSION 3.10)

project(hcl VERSION 1.0.0 DESCRIPTION "HCL: A library for flexible distributed data containers")

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -gdwarf-2 -g3 -lm -lc -ldl -pthread")
set(RPC_LIB_FLAGS "")
include_directories(${CMAKE_PREFIX_PATH}/include)
add_definitions(-w)
option(HCL_ENABLE_RPCLIB "allow hcl to use RPCLIB" OFF)
option(HCL_ENABLE_THALLIUM_TCP "allow hcl to use Thallium TCP" ON)
option(HCL_ENABLE_THALLIUM_ROCE "allow hcl to use Thallium ROCe" OFF)
option(HCL_TIMER "Show timing information of library calls." OFF)
option(HCL_TRACE "Show traces of library calls." OFF)

set(HCL_CMAKE_DIR ${CMAKE_SOURCE_DIR}/CMake)
set(CMAKE_MODULE_PATH ${HCL_CMAKE_DIR} ${CMAKE_MODULE_PATH})
set(CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/.spack-env/view ${CMAKE_PREFIX_PATH})
if (HCL_ENABLE_RPCLIB)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DHCL_ENABLE_RPCLIB")
    message(STATUS "HCL_ENABLE_RPCLIB: ${HCL_ENABLE_RPCLIB}")
elseif (HCL_ENABLE_THALLIUM_TCP)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DHCL_ENABLE_THALLIUM_TCP")
    message(STATUS "HCL_ENABLE_THALLIUM_TCP: ${HCL_ENABLE_THALLIUM_TCP}")
elseif(HCL_ENABLE_THALLIUM_ROCE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DHCL_ENABLE_THALLIUM_ROCE")
    message(STATUS "HCL_ENABLE_THALLIUM_ROCE: ${HCL_ENABLE_THALLIUM_ROCE}")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

include(GNUInstallDirs)
include_directories(src/hcl)
include_directories(src)
include_directories(include)
set(HCL_SRC  include/hcl/common/singleton.h
                include/hcl/common/enumerations.h
                include/hcl/common/macros.h
                include/hcl/common/configuration_manager.h
                include/hcl/common/constants.h
                include/hcl/common/typedefs.h
                include/hcl/common/data_structures.h
                include/hcl/communication/rpc_lib.h
                include/hcl/unordered_map/unordered_map.h
                include/hcl/map/map.h
                include/hcl/multimap/multimap.h
                include/hcl/clock/global_clock.h
                include/hcl/queue/queue.h
                include/hcl/priority_queue/priority_queue.h
                include/hcl/set/set.h
                include/hcl/sequencer/global_sequence.h
                include/hcl/communication/rpc_factory.h include/hcl/common/container.h)

add_library(${PROJECT_NAME} SHARED ${HCL_SRC})

set_target_properties(${PROJECT_NAME} PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER include/hcl.h)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(HCL_TIMER)
  target_compile_definitions(${PROJECT_NAME} PRIVATE HCL_TIMER)
endif()

if(HCL_TRACE)
  target_compile_definitions(${PROJECT_NAME} PRIVATE HCL_TRACE)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

configure_file(hcl.pc.in hcl.pc @ONLY)

if (HCL_ENABLE_RPCLIB)
    find_package(rpclib REQUIRED)
    set(RPC_LIB_FLAGS rpclib::rpc ${RPC_LIB_FLAGS})
endif ()
if (HCL_ENABLE_THALLIUM_ROCE OR HCL_ENABLE_THALLIUM_TCP)
    find_package(thallium REQUIRED)
    include_directories(${THALLIUM_INCLUDE_DIRS})
    set(RPC_LIB_FLAGS thallium ${RPC_LIB_FLAGS})
endif ()

find_package(MPI REQUIRED COMPONENTS CXX C)
if (MPI_FOUND)
    message(STATUS "found mpi.h at ${MPI_CXX_INCLUDE_DIRS}")
endif ()
include_directories(${MPI_CXX_INCLUDE_DIRS})
set(LIB_FLAGS ${RPC_LIB_FLAGS} ${MPI_CXX_LIBRARIES} -lpthread -lrt)

# libs for NATS tests
#set(LIB_FLAGS ${LIB_FLAGS} -lnats)

# libs for Redis tests
#set(LIB_FLAGS ${LIB_FLAGS} -lev -L/home/kbateman/install/lib/ -L/home/kbateman/install/lib64/ -lhiredis -lredox)

target_link_libraries(hcl ${LIB_FLAGS})

install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

option(BUILD_TEST "Build the unit tests" ON)
if(BUILD_TEST)
    enable_testing()
    add_subdirectory(test)
endif()
