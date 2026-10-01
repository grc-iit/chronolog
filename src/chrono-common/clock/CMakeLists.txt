add_library(chronolog_common_clock STATIC SystemClock.cpp KernelClock.cpp)
        target_include_directories(chronolog_common_clock PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")
                target_link_libraries(chronolog_common_clock PUBLIC chronolog_contracts
                                              absl::status absl::statusor PRIVATE chronolog_warnings Threads::Threads)
