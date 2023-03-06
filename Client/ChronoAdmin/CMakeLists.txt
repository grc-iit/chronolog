
add_executable(ChronoAdmin client_admin.cpp)
target_include_directories(ChronoAdmin PRIVATE ../include
			   ../../ChronoAPI/ChronoLog/include
				)
target_link_libraries(ChronoAdmin ChronoLogClient_lib)

add_executable(ChronoAdmin_Sim client_admin_sim.cpp)
target_include_directories(ChronoAdmin_Sim PRIVATE ../include 
			       ../../ChronoAPI/ChronoLog/include)
target_link_libraries(ChronoAdmin_Sim ChronoLogClient_lib)		       
