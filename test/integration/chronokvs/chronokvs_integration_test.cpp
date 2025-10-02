#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <iomanip>
#include "chronokvs.h"

/**
 * ChronoKVS API Integration Test
 * 
 * This test verifies the ChronoKVS API functionality with 3 modular tests:
 * 1. Put: Write 1000 values to a single story
 * 2. Get History: Retrieve all values from the story
 * 3. Get: Retrieve 10 random specific values by timestamp
 */

class ChronoKVSTest {
private:
    chronokvs::ChronoKVS kvs;
    const std::string test_key = "chronokvs_test_story";
    const int num_values = 1000;
    const int num_random_gets = 10;
    std::vector<std::uint64_t> timestamps;
    std::vector<std::string> values;
    std::vector<int> random_indices;

    void printHeader(const std::string& title) {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "  " << title << std::endl;
        std::cout << std::string(60, '=') << std::endl;
    }

    void printProgressBar(int current, int total, const std::string& message) {
        float progress = (float)current / total;
        int barWidth = 50;
        int pos = barWidth * progress;
        
        std::cout << "\r" << "  " << message << " [";
        for (int i = 0; i < barWidth; ++i) {
            if (i < pos) std::cout << "█";
            else if (i == pos) std::cout << "▶";
            else std::cout << "░";
        }
        std::cout << "] " << std::setw(3) << int(progress * 100.0) << "%";
        std::cout.flush();
    }

    void printTestResult(const std::string& testName, bool passed, const std::string& details = "") {
        std::cout << "  " << (passed ? "✅" : "❌") << " " << testName;
        if (!details.empty()) {
            std::cout << " - " << details;
        }
        std::cout << std::endl;
    }

    void printSeparator() {
        std::cout << std::string(60, '-') << std::endl;
    }

public:
    bool test1_put() {
        printHeader("TEST 1: PUT OPERATIONS");
        std::cout << "  Writing " << num_values << " values to story '" << test_key << "'" << std::endl;
        std::cout << "  Target: " << num_values << " successful put operations" << std::endl;
        printSeparator();
        
        // Generate test data
        values.clear();
        timestamps.clear();
        for (int i = 0; i < num_values; i++) {
            values.push_back("value_" + std::to_string(i));
        }

        auto start_time = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_values; i++) {
            std::uint64_t timestamp = kvs.put(test_key, values[i]);
            timestamps.push_back(timestamp);
            
            if ((i + 1) % 100 == 0 || i < 5) {
                printProgressBar(i + 1, num_values, "Writing values");
            }
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        std::cout << "\n" << std::endl;
        printTestResult("Put Operations", timestamps.size() == static_cast<size_t>(num_values), 
                       std::to_string(timestamps.size()) + "/" + std::to_string(num_values) + " successful");
        std::cout << "  Performance: " << duration.count() << " ms (" 
                  << std::fixed << std::setprecision(1) << (num_values * 1000.0 / duration.count()) 
                  << " ops/sec)" << std::endl;
        
        return timestamps.size() == static_cast<size_t>(num_values);
    }

    void waitForDataPropagation() {
        printHeader("DATA PROPAGATION WAIT");
        std::cout << "  ChronoLog requires 1-2 minutes for data to be available for retrieval" << std::endl;
        std::cout << "  This ensures data is properly committed to persistent storage" << std::endl;
        printSeparator();
        
        const int total_seconds = 120;
        const int update_interval = 1;
        
        for (int elapsed = 0; elapsed < total_seconds; elapsed += update_interval) {
            printProgressBar(elapsed, total_seconds, "Data propagation");
            std::this_thread::sleep_for(std::chrono::seconds(update_interval));
        }
        
        std::cout << "\n" << std::endl;
        printTestResult("Data Propagation", true, "Wait period completed");
        std::cout << "  Data should now be available for retrieval operations" << std::endl;
    }

    bool test2_get_history() {
        printHeader("TEST 2: GET HISTORY OPERATIONS");
        std::cout << "  Retrieving all values from story '" << test_key << "'" << std::endl;
        std::cout << "  Target: At least " << num_values << " values retrieved" << std::endl;
        printSeparator();
        
        try {
            auto history = kvs.get_history(test_key);
            
            bool success = history.size() >= static_cast<size_t>(num_values);
            printTestResult("Get History Operations", success, 
                           std::to_string(history.size()) + " values retrieved");
            
            if (history.size() < static_cast<size_t>(num_values)) {
                std::cout << "  ⚠️  Warning: Expected " << num_values << " values, got " << history.size() << std::endl;
                std::cout << "  This may indicate data propagation issues or previous test data" << std::endl;
            } else {
                std::cout << "  ✓ History contains " << history.size() << " values (expected at least " << num_values << ")" << std::endl;
            }
            
            // Store random indices for test 3
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, std::min(num_values - 1, (int)history.size() - 1));
            
            random_indices.clear();
            for (int i = 0; i < num_random_gets; i++) {
                random_indices.push_back(dis(gen));
            }
            
            std::cout << "  Selected " << num_random_gets << " random indices for Test 3" << std::endl;
            
            return success;
        } catch (const std::exception& e) {
            printTestResult("Get History Operations", false, "Exception: " + std::string(e.what()));
            return false;
        }
    }

    bool test3_get() {
        printHeader("TEST 3: GET OPERATIONS");
        std::cout << "  Retrieving " << num_random_gets << " random specific values by timestamp" << std::endl;
        std::cout << "  Target: " << num_random_gets << " successful exact retrievals" << std::endl;
        printSeparator();
        
        int successful_gets = 0;
        
        for (int i = 0; i < num_random_gets; i++) {
            int idx = random_indices[i];
            std::string expected_value = values[idx];
            std::uint64_t timestamp = timestamps[idx];
            
            try {
                std::string retrieved_value = kvs.get(test_key, timestamp);
                if (retrieved_value == expected_value) {
                    successful_gets++;
                    std::cout << "  ✓ Get " << std::setw(2) << (i+1) << "/" << num_random_gets 
                             << ": timestamp " << std::setw(12) << timestamp 
                             << " -> " << retrieved_value << std::endl;
                } else {
                    std::cout << "  ✗ Get " << std::setw(2) << (i+1) << "/" << num_random_gets 
                             << ": timestamp " << std::setw(12) << timestamp 
                             << " -> expected '" << expected_value 
                             << "' but got '" << retrieved_value << "'" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cout << "  ✗ Get " << std::setw(2) << (i+1) << "/" << num_random_gets 
                         << ": timestamp " << std::setw(12) << timestamp 
                         << " failed with " << e.what() << std::endl;
            }
        }
        
        std::cout << std::endl;
        bool success = successful_gets == num_random_gets;
        printTestResult("Get Operations", success, 
                       std::to_string(successful_gets) + "/" + std::to_string(num_random_gets) + " successful");
        
        if (success) {
            std::cout << "  ✓ All exact timestamp retrievals successful" << std::endl;
        } else {
            std::cout << "  ⚠️  Some exact retrievals failed - this may indicate timing precision issues" << std::endl;
        }
        
        return success;
    }

    bool runAllTests() {
        // Main header
        std::cout << "\n" << std::string(80, '=') << std::endl;
        std::cout << "                    CHRONOKVS API INTEGRATION TEST" << std::endl;
        std::cout << "                    ==============================" << std::endl;
        std::cout << "  Purpose: Verify ChronoKVS API functionality for GitHub Actions" << std::endl;
        std::cout << "  Tests:   Put Operations, Data Propagation, Get History, Get Operations" << std::endl;
        std::cout << std::string(80, '=') << std::endl;
        
        bool test1_result = false;
        bool test2_result = false;
        bool test3_result = false;
        
        // Test 1: Put
        test1_result = test1_put();
        
        // Wait for data propagation (only if Test 1 passed)
        if (test1_result) {
            waitForDataPropagation();
        } else {
            printHeader("DATA PROPAGATION WAIT");
            std::cout << "  ⚠️  Skipping data propagation wait due to Test 1 failure" << std::endl;
            std::cout << "  Subsequent tests may not have data to work with" << std::endl;
        }
        
        // Test 2: Get History
        test2_result = test2_get_history();
        
        // Test 3: Get (only if we have data from previous tests)
        if (test1_result && timestamps.size() > 0) {
            test3_result = test3_get();
        } else {
            printHeader("TEST 3: GET OPERATIONS");
            std::cout << "  ⚠️  Skipping Test 3 (Get) - no data available from Test 1" << std::endl;
            std::cout << "  This test requires successful put operations to proceed" << std::endl;
            test3_result = false;
        }
        
        // Final summary
        printHeader("FINAL TEST RESULTS");
        std::cout << "  Test 1 (Put):        " << (test1_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        std::cout << "  Test 2 (Get History): " << (test2_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        std::cout << "  Test 3 (Get):        " << (test3_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        printSeparator();
        
        int passed_tests = (test1_result ? 1 : 0) + (test2_result ? 1 : 0) + (test3_result ? 1 : 0);
        std::cout << "  Overall: " << passed_tests << "/3 tests passed" << std::endl;
        
        if (passed_tests == 3) {
            std::cout << "\n  🎉 ALL CHRONOKVS API TESTS PASSED! 🎉" << std::endl;
            std::cout << "  The ChronoKVS integration is working correctly and ready for GitHub Actions." << std::endl;
        } else if (passed_tests > 0) {
            std::cout << "\n  ⚠️  PARTIAL SUCCESS: " << passed_tests << "/3 tests passed" << std::endl;
            std::cout << "  Some ChronoKVS functionality is working, but there may be issues to investigate." << std::endl;
        } else {
            std::cout << "\n  ❌ ALL TESTS FAILED" << std::endl;
            std::cout << "  ChronoKVS integration has critical issues that need to be resolved." << std::endl;
        }
        
        std::cout << std::string(80, '=') << std::endl;
        
        return passed_tests == 3;
    }
};

int main() {
    try {
        ChronoKVSTest test;
        bool success = test.runAllTests();
        return success ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "\n" << std::string(80, '!') << std::endl;
        std::cerr << "  CRITICAL ERROR: Test failed with exception" << std::endl;
        std::cerr << "  " << e.what() << std::endl;
        std::cerr << "  This indicates a serious issue with the ChronoKVS integration." << std::endl;
        std::cerr << std::string(80, '!') << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "\n" << std::string(80, '!') << std::endl;
        std::cerr << "  CRITICAL ERROR: Test failed with unknown exception" << std::endl;
        std::cerr << "  This indicates an unexpected error in the ChronoKVS integration." << std::endl;
        std::cerr << std::string(80, '!') << std::endl;
        return 1;
    }
}
