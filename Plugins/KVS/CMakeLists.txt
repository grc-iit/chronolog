
include_directories("../emu/include")
include_directories("include")

set(KVSLIBNAME  "KVSLib")
add_library(${KVSLIBNAME} SHARED srcs/KeyValueStore.cpp srcs/KeyValueStoreIO.cpp srcs/util.cpp ../emu/srcs/city.cpp)
include_directories(${KVSLIBNAME} include)
target_link_libraries(${KVSLIBNAME} ${MPI_CXX_LIBRARIES} thallium ${HDF5_C_LIBRARIES} ${Boost_LIBRARIES} -lpthread -lrt)
set_target_properties(${KVSLIBNAME} PROPERTIES LINKER_LANGUAGE CXX)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS ${KVSLIBNAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
add_subdirectory(KVSClient)
