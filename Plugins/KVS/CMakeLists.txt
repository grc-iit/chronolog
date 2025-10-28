
include_directories("../emu/include")
include_directories("include")

set(KVSLIBNAME "KVSLib")
add_library(${KVSLIBNAME} SHARED srcs/KeyValueStore.cpp srcs/KeyValueStoreIO.cpp srcs/util.cpp ../emu/srcs/city.cpp)
include_directories(${KVSLIBNAME} include)
target_link_libraries(${KVSLIBNAME} ${MPI_CXX_LIBRARIES} thallium ${HDF5_C_LIBRARIES} ${Boost_LIBRARIES} -lpthread -lrt)
set_target_properties(${KVSLIBNAME} PROPERTIES LINKER_LANGUAGE CXX)
# Set PUBLIC_HEADER property for automatic installation
file(GLOB HEADER_FILES "include/*.h")
set_target_properties(${KVSLIBNAME} PROPERTIES
    PUBLIC_HEADER "${HEADER_FILES}"
)

chronolog_install_target(${KVSLIBNAME})
add_subdirectory(KVSClient)
