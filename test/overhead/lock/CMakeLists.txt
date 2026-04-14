add_executable(lock_overhead_test lock_overhead_test.cpp)
target_link_libraries(lock_overhead_test
    PRIVATE
        pthread
        rt
)

add_test(NAME Overhead_Lock_ContentionBenchmark COMMAND lock_overhead_test)

