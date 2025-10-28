
include_directories("include")
add_executable(engine srcs/main.cpp srcs/city.cpp srcs/rw.cpp srcs/distributed_sort.cpp srcs/rw_request.cpp srcs/query_parser.cpp srcs/nvme_buffer.cpp srcs/query_engine.cpp srcs/write_buffer.cpp srcs/process.cpp srcs/event.cpp)
#add_executable(post_processing srcs/post_processing.cpp)
add_executable(hdf5_workload hdf5_workload3.cpp)
add_library(${PROJECT_NAME} SHARED srcs)
include_directories(${PROJECT_NAME} include)
#target_link_libraries(${PROJECT_NAME} ${MPI_CXX_LIBRARIES} thallium ${HDF5_C_LIBRARIES} -L${HDF5_VOL_ASYNC}/lib -lasynchdf5 -lh5async ${Boost_LIBRARIES} -lpthread -lrt)
target_link_libraries(${PROJECT_NAME} ${MPI_CXX_LIBRARIES} thallium ${HDF5_C_LIBRARIES} ${Boost_LIBRARIES} -lpthread -lrt)
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)
# Set PUBLIC_HEADER property for automatic installation
file(GLOB HEADER_FILES "include/*.h")
set_target_properties(${PROJECT_NAME} PROPERTIES
    PUBLIC_HEADER "${HEADER_FILES}"
)

chronolog_install_target(${PROJECT_NAME})
#target_link_libraries(engine ${MPI_CXX_LIBRARIES} thallium ${HDF5_C_LIBRARIES} -L${HDF5_VOL_ASYNC}/lib -lasynchdf5 -lh5async ${Boost_LIBRARIES} -lpthread -lrt)
target_link_libraries(engine ${MPI_CXX_LIBRARIES} thallium ${HDF5_C_LIBRARIES} ${Boost_LIBRARIES} -lpthread -lrt)
#target_link_libraries(post_processing ${MPI_CXX_LIBRARIES} thallium ${HDF5_C_LIBRARIES} -L${HDF5_VOL_ASYNC}/lib -lasynchdf5 -lh5async ${Boost_LIBRARIES} -lpthread -lrt)
#target_link_libraries(hdf5_workload ${HDF5_C_LIBRARIES} -L${HDF5_VOL_ASYNC}/lib -lasynchdf5 -lh5async -lm)
target_link_libraries(hdf5_workload ${HDF5_C_LIBRARIES} -lm)
set_target_properties(engine PROPERTIES FOLDER emu)
#set_target_properties(post_processing PROPERTIES FOLDER emu)
