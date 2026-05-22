cmake_minimum_required(VERSION 3.14)
project(ingestion_kernel_bench CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Install destination — use parent project's benchmark dir when available,
# fall back to a relative path for standalone cmake installs.
# ---------------------------------------------------------------------------
if(DEFINED CHRONOLOG_INSTALL_BENCHMARK_DIR)
    set(_bench_install_dir "${CHRONOLOG_INSTALL_BENCHMARK_DIR}")
else()
    set(_bench_install_dir "benchmark")
endif()

# ---------------------------------------------------------------------------
# Local benchmark — no network dependency, builds anywhere with just g++.
# Installed name: chrono-ingestion-bench
# ---------------------------------------------------------------------------
add_executable(ingestion_kernel_bench ingestion_kernel_bench.cpp)
set_target_properties(ingestion_kernel_bench PROPERTIES OUTPUT_NAME chrono-ingestion-bench)
target_include_directories(ingestion_kernel_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ingestion_kernel_bench PRIVATE Threads::Threads)
target_compile_options(ingestion_kernel_bench PRIVATE -Wall -Wextra -O3)

if(COMMAND chronolog_install_target)
    chronolog_install_target(ingestion_kernel_bench benchmark)
else()
    install(TARGETS ingestion_kernel_bench RUNTIME DESTINATION "${_bench_install_dir}")
endif()

# ---------------------------------------------------------------------------
# Network benchmarks — require Thallium (Mercury + Margo + Argobots).
# Build with:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target ingestion_keeper_server ingestion_keeper_client
# If Thallium is not found these targets are silently skipped.
# Installed names: chrono-ingestion-bench-server, chrono-ingestion-bench-client
# ---------------------------------------------------------------------------
find_package(Thallium QUIET)

if(Thallium_FOUND)
    message(STATUS "Thallium found — building network ingestion benchmarks")

    # Server: registers record_event (send/recv) and record_events_bulk (RDMA)
    add_executable(ingestion_keeper_server ingestion_keeper_server.cpp)
    set_target_properties(ingestion_keeper_server PROPERTIES OUTPUT_NAME chrono-ingestion-bench-server)
    target_include_directories(ingestion_keeper_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}       # concurrentqueue.h
    )
    target_link_libraries(ingestion_keeper_server PRIVATE thallium Threads::Threads)
    target_compile_options(ingestion_keeper_server PRIVATE -Wall -Wextra -O3)

    # Client: N pthread producers, sendrecv or RDMA bulk
    add_executable(ingestion_keeper_client ingestion_keeper_client.cpp)
    set_target_properties(ingestion_keeper_client PROPERTIES OUTPUT_NAME chrono-ingestion-bench-client)
    target_include_directories(ingestion_keeper_client PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}       # concurrentqueue.h (for EventPOD shared layout)
    )
    target_link_libraries(ingestion_keeper_client PRIVATE thallium Threads::Threads)
    target_compile_options(ingestion_keeper_client PRIVATE -Wall -Wextra -O3)

    if(COMMAND chronolog_install_target)
        chronolog_install_target(ingestion_keeper_server benchmark)
        chronolog_install_target(ingestion_keeper_client benchmark)
    else()
        install(TARGETS ingestion_keeper_server ingestion_keeper_client
                RUNTIME DESTINATION "${_bench_install_dir}")
    endif()
else()
    message(STATUS "Thallium NOT found — skipping ingestion_keeper_server/client")
endif()
