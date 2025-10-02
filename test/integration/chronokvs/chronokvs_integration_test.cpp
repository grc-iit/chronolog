#include <string>
#include <iostream>
#include <vector>
#include <cassert>
#include <chrono>
#include <thread>
#include <random>
#include "chronokvs.h"

/**
 * Integration test for ChronoKVS
 * 
 * This test verifies the complete functionality of ChronoKVS including:
 * - Basic put/get operations
 * - Historical data retrieval
 * - Timestamp ordering
 * - Concurrent operations
 * - Error handling
 * - Performance characteristics
 */

class ChronoKVSIntegrationTest {
private:
    chronokvs::ChronoKVS kvs;
    std::vector<std::uint64_t> test_timestamps;
    int test_count = 0;
    int passed_tests = 0;

    void assert_test(bool condition, const std::string& test_name) {
        test_count++;
        if (condition) {
            passed_tests++;
            std::cout << "✓ " << test_name << std::endl;
        } else {
            std::cout << "✗ " << test_name << std::endl;
        }
    }

    void assert_equals(const std::string& expected, const std::string& actual, const std::string& test_name) {
        assert_test(expected == actual, test_name + " (expected: '" + expected + "', got: '" + actual + "')");
    }

    void assert_not_empty(const std::string& value, const std::string& test_name) {
        assert_test(!value.empty(), test_name + " (value should not be empty)");
    }

    void assert_empty(const std::string& value, const std::string& test_name) {
        assert_test(value.empty(), test_name + " (value should be empty)");
    }

    void assert_size(size_t expected, size_t actual, const std::string& test_name) {
        assert_test(expected == actual, test_name + " (expected size: " + std::to_string(expected) + 
                   ", got: " + std::to_string(actual) + ")");
    }

public:
    void run_all_tests() {
        std::cout << "Starting ChronoKVS Integration Tests..." << std::endl;
        std::cout << "========================================" << std::endl;

        test_basic_operations();
        test_historical_data();
        test_timestamp_ordering();
        test_concurrent_operations();
        test_error_handling();
        test_performance();
        test_edge_cases();

        std::cout << "========================================" << std::endl;
        std::cout << "Test Results: " << passed_tests << "/" << test_count << " tests passed" << std::endl;
        
        if (passed_tests == test_count) {
            std::cout << "🎉 All tests passed!" << std::endl;
        } else {
            std::cout << "❌ Some tests failed!" << std::endl;
            exit(1);
        }
    }

    void test_basic_operations() {
        std::cout << "\n--- Testing Basic Operations ---" << std::endl;

        // Test basic put operation
        std::string key1 = "test_key_1";
        std::string value1 = "test_value_1";
        std::uint64_t timestamp1 = kvs.put(key1, value1);
        test_timestamps.push_back(timestamp1);
        
        assert_test(timestamp1 > 0, "Put operation returns valid timestamp");
        assert_not_empty(std::to_string(timestamp1), "Timestamp is not zero");

        // Test basic get operation
        std::string retrieved_value = kvs.get(key1, timestamp1);
        assert_equals(value1, retrieved_value, "Get operation returns correct value");

        // Test multiple puts for same key
        std::string value2 = "test_value_2";
        std::uint64_t timestamp2 = kvs.put(key1, value2);
        test_timestamps.push_back(timestamp2);
        
        assert_test(timestamp2 > timestamp1, "Second timestamp is greater than first");
        
        std::string retrieved_value2 = kvs.get(key1, timestamp2);
        assert_equals(value2, retrieved_value2, "Get operation returns latest value");

        // Test different keys
        std::string key2 = "test_key_2";
        std::string value3 = "test_value_3";
        std::uint64_t timestamp3 = kvs.put(key2, value3);
        test_timestamps.push_back(timestamp3);
        
        std::string retrieved_value3 = kvs.get(key2, timestamp3);
        assert_equals(value3, retrieved_value3, "Different keys work independently");
    }

    void test_historical_data() {
        std::cout << "\n--- Testing Historical Data ---" << std::endl;

        std::string key = "history_key";
        
        // Insert multiple values for the same key
        std::vector<std::string> values = {"v1", "v2", "v3", "v4", "v5"};
        std::vector<std::uint64_t> timestamps;
        
        for (const auto& value : values) {
            std::uint64_t ts = kvs.put(key, value);
            timestamps.push_back(ts);
            test_timestamps.push_back(ts);
        }

        // Test get_history
        auto history = kvs.get_history(key);
        assert_size(values.size(), history.size(), "History contains all values");
        
        // Verify history is ordered by timestamp (ascending)
        for (size_t i = 1; i < history.size(); i++) {
            assert_test(history[i].timestamp > history[i-1].timestamp, 
                       "History is ordered by timestamp");
        }

        // Verify all values are present
        for (size_t i = 0; i < values.size(); i++) {
            assert_equals(values[i], history[i].value, 
                         "History value " + std::to_string(i) + " matches");
        }

        // Test get at specific timestamps
        for (size_t i = 0; i < values.size(); i++) {
            std::string retrieved = kvs.get(key, timestamps[i]);
            assert_equals(values[i], retrieved, 
                         "Get at timestamp " + std::to_string(i) + " returns correct value");
        }
    }

    void test_timestamp_ordering() {
        std::cout << "\n--- Testing Timestamp Ordering ---" << std::endl;

        std::string key = "ordering_key";
        std::vector<std::uint64_t> timestamps;
        
        // Insert values with small delays to ensure different timestamps
        for (int i = 0; i < 5; i++) {
            std::uint64_t ts = kvs.put(key, "value_" + std::to_string(i));
            timestamps.push_back(ts);
            test_timestamps.push_back(ts);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Verify timestamps are in ascending order
        for (size_t i = 1; i < timestamps.size(); i++) {
            assert_test(timestamps[i] > timestamps[i-1], 
                       "Timestamps are in ascending order");
        }

        // Test that get with intermediate timestamp returns correct value
        std::string mid_value = kvs.get(key, timestamps[2]);
        assert_equals("value_2", mid_value, "Get at intermediate timestamp works");
    }

    void test_concurrent_operations() {
        std::cout << "\n--- Testing Concurrent Operations ---" << std::endl;

        std::string key = "concurrent_key";
        std::vector<std::thread> threads;
        std::vector<std::uint64_t> thread_timestamps(5);
        std::mutex mutex;

        // Create multiple threads that insert values
        for (int i = 0; i < 5; i++) {
            threads.emplace_back([this, &key, &thread_timestamps, &mutex, i]() {
                std::uint64_t ts = kvs.put(key, "concurrent_value_" + std::to_string(i));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    thread_timestamps[i] = ts;
                    test_timestamps.push_back(ts);
                }
            });
        }

        // Wait for all threads to complete
        for (auto& thread : threads) {
            thread.join();
        }

        // Verify all values were inserted
        auto history = kvs.get_history(key);
        assert_size(5, history.size(), "All concurrent values were inserted");

        // Verify all timestamps are unique
        std::set<std::uint64_t> unique_timestamps(thread_timestamps.begin(), thread_timestamps.end());
        assert_size(5, unique_timestamps.size(), "All timestamps are unique");
    }

    void test_error_handling() {
        std::cout << "\n--- Testing Error Handling ---" << std::endl;

        // Test get with non-existent key
        std::string non_existent = kvs.get("non_existent_key", 12345);
        assert_empty(non_existent, "Get non-existent key returns empty");

        // Test get with non-existent timestamp
        std::string key = "error_test_key";
        std::uint64_t valid_ts = kvs.put(key, "valid_value");
        test_timestamps.push_back(valid_ts);
        
        std::string invalid_result = kvs.get(key, valid_ts + 1000);
        assert_empty(invalid_result, "Get with invalid timestamp returns empty");

        // Test get_history with non-existent key
        auto empty_history = kvs.get_history("non_existent_key");
        assert_size(0, empty_history.size(), "History of non-existent key is empty");
    }

    void test_performance() {
        std::cout << "\n--- Testing Performance ---" << std::endl;

        const int num_operations = 1000;
        std::string key = "perf_key";
        
        // Test put performance
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < num_operations; i++) {
            std::uint64_t ts = kvs.put(key, "perf_value_" + std::to_string(i));
            test_timestamps.push_back(ts);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "  Put " << num_operations << " operations took " << duration.count() << " ms" << std::endl;
        assert_test(duration.count() < 5000, "Put operations complete in reasonable time");

        // Test get performance
        start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < num_operations; i++) {
            std::string value = kvs.get(key, test_timestamps[test_timestamps.size() - num_operations + i]);
            assert_not_empty(value, "Get performance test value retrieval");
        }
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "  Get " << num_operations << " operations took " << duration.count() << " ms" << std::endl;
        assert_test(duration.count() < 5000, "Get operations complete in reasonable time");

        // Test get_history performance
        start = std::chrono::high_resolution_clock::now();
        auto history = kvs.get_history(key);
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "  Get history took " << duration.count() << " ms" << std::endl;
        assert_test(duration.count() < 1000, "Get history completes in reasonable time");
    }

    void test_edge_cases() {
        std::cout << "\n--- Testing Edge Cases ---" << std::endl;

        // Test empty key
        std::uint64_t ts1 = kvs.put("", "empty_key_value");
        test_timestamps.push_back(ts1);
        std::string empty_key_result = kvs.get("", ts1);
        assert_equals("empty_key_value", empty_key_result, "Empty key works");

        // Test empty value
        std::uint64_t ts2 = kvs.put("empty_value_key", "");
        test_timestamps.push_back(ts2);
        std::string empty_value_result = kvs.get("empty_value_key", ts2);
        assert_equals("", empty_value_result, "Empty value works");

        // Test very long key and value
        std::string long_key(1000, 'a');
        std::string long_value(10000, 'b');
        std::uint64_t ts3 = kvs.put(long_key, long_value);
        test_timestamps.push_back(ts3);
        std::string long_result = kvs.get(long_key, ts3);
        assert_equals(long_value, long_result, "Long key and value work");

        // Test special characters
        std::string special_key = "key!@#$%^&*()_+-=[]{}|;':\",./<>?";
        std::string special_value = "value\t\n\r\0\x01\x02";
        std::uint64_t ts4 = kvs.put(special_key, special_value);
        test_timestamps.push_back(ts4);
        std::string special_result = kvs.get(special_key, ts4);
        assert_equals(special_value, special_result, "Special characters work");
    }
};

int main() {
    try {
        ChronoKVSIntegrationTest test;
        test.run_all_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Test failed with unknown exception" << std::endl;
        return 1;
    }
}
