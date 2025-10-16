set(KVSLIBNAME "KVSLib")
add_executable(pubsubclient srcs/client_program.cpp srcs/PSClient.cpp srcs/MessageCache.cpp)

# Set include directories for the executable
target_include_directories(pubsubclient PRIVATE 
    include
    ../PluginKVS/include
    ../emu/include
)

target_link_libraries(pubsubclient 
    thallium 
    ${MPI_CXX_LIBRARIES} 
    ${HDF5_C_LIBRARIES} 
    ${Boost_LIBRARIES} 
    -lpthread 
    -lrt 
    ${KVSLIBNAME} 
    -lm
)
set_target_properties(pubsubclient PROPERTIES FOLDER pubsubclient)

