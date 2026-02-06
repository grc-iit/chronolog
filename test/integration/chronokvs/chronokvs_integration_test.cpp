#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "chronokvs.h"

/**
 * ChronoKVS API Integration Test
 *
 * This test verifies the ChronoKVS API functionality with 6 modular tests:
 * 1. Put: Write 1000 values to a single story
 * 2. Get History: Retrieve all values from the story
 * 3. Get: Retrieve 10 random specific values by timestamp
 * 4. Get Range: Retrieve events within specific time ranges
 * 5. Get Earliest: Retrieve the earliest event for a key
 * 6. Get Latest: Retrieve the latest event for a key
 */

class ChronoKVSTest
{
private:
    chronokvs::ChronoKVS kvs;
    const std::string test_key = "chronokvs_test_story";
    const int num_values = 1000;
    const int num_random_gets = 10;
    std::vector<std::uint64_t> timestamps;
    std::vector<std::string> values;
    std::vector<int> random_indices;
    std::vector<chronokvs::EventData> history_events;

    void printHeader(const std::string& title)
    {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "  " << title << std::endl;
        std::cout << std::string(60, '=') << std::endl;
    }

    void printProgressBar(int current, int total, const std::string& message)
    {
        float progress = (float)current / total;
        int barWidth = 50;
        int pos = barWidth * progress;

        std::cout << "\r" << "  " << message << " [";
        for(int i = 0; i < barWidth; ++i)
        {
            if(i < pos)
                std::cout << "█";
            else if(i == pos)
                std::cout << "▶";
            else
                std::cout << "░";
        }
        std::cout << "] " << std::setw(3) << int(progress * 100.0) << "%";
        std::cout.flush();
    }

    void printTestResult(const std::string& testName, bool passed, const std::string& details = "")
    {
        std::cout << "  " << (passed ? "✅" : "❌") << " " << testName;
        if(!details.empty())
        {
            std::cout << " - " << details;
        }
        std::cout << std::endl;
    }

    void printSeparator() { std::cout << std::string(60, '-') << std::endl; }

public:
    bool test1_put()
    {
        printHeader("TEST 1: PUT OPERATIONS");
        std::cout << "  Writing " << num_values << " values to story '" << test_key << "'" << std::endl;
        std::cout << "  Target: " << num_values << " successful put operations" << std::endl;
        printSeparator();

        // Generate test data
        values.clear();
        timestamps.clear();
        for(int i = 0; i < num_values; i++) { values.push_back("value_" + std::to_string(i)); }

        auto start_time = std::chrono::high_resolution_clock::now();

        for(int i = 0; i < num_values; i++)
        {
            std::uint64_t timestamp = kvs.put(test_key, values[i]);
            timestamps.push_back(timestamp);

            if((i + 1) % 100 == 0 || i < 5)
            {
                printProgressBar(i + 1, num_values, "Writing values");
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        std::cout << "\n" << std::endl;
        printTestResult("Put Operations",
                        timestamps.size() == static_cast<size_t>(num_values),
                        std::to_string(timestamps.size()) + "/" + std::to_string(num_values) + " successful");
        std::cout << "  Performance: " << duration.count() << " ms (" << std::fixed << std::setprecision(1)
                  << (num_values * 1000.0 / duration.count()) << " ops/sec)" << std::endl;

        return timestamps.size() == static_cast<size_t>(num_values);
    }

    void waitForDataPropagation()
    {
        printHeader("DATA PROPAGATION WAIT");
        std::cout << "  ChronoLog requires 1-2 minutes for data to be available for retrieval" << std::endl;
        std::cout << "  This ensures data is properly committed to persistent storage" << std::endl;
        printSeparator();

        const int total_seconds = 120;
        const int update_interval = 1;

        for(int elapsed = 0; elapsed < total_seconds; elapsed += update_interval)
        {
            printProgressBar(elapsed, total_seconds, "Data propagation");
            std::this_thread::sleep_for(std::chrono::seconds(update_interval));
        }

        std::cout << "\n" << std::endl;
        printTestResult("Data Propagation", true, "Wait period completed");
        std::cout << "  Data should now be available for retrieval operations" << std::endl;
    }

    bool test2_get_history()
    {
        printHeader("TEST 2: GET HISTORY OPERATIONS");
        std::cout << "  Retrieving all values from story '" << test_key << "'" << std::endl;
        std::cout << "  Target: At least " << num_values << " values retrieved" << std::endl;
        printSeparator();

        try
        {
            auto history = kvs.get_history(test_key);
            history_events = history; // Store for use in subsequent tests

            bool success = history.size() >= static_cast<size_t>(num_values);
            printTestResult("Get History Operations", success, std::to_string(history.size()) + " values retrieved");

            if(history.size() < static_cast<size_t>(num_values))
            {
                std::cout << "  ⚠️  Warning: Expected " << num_values << " values, got " << history.size() << std::endl;
                std::cout << "  This may indicate data propagation issues or previous test data" << std::endl;
            }
            else
            {
                std::cout << "  ✓ History contains " << history.size() << " values (expected at least " << num_values
                          << ")" << std::endl;
            }

            // Store random indices for test 3
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(0, std::min(num_values - 1, (int)history.size() - 1));

            random_indices.clear();
            for(int i = 0; i < num_random_gets; i++) { random_indices.push_back(dis(gen)); }

            std::cout << "  Selected " << num_random_gets << " random indices for Test 3" << std::endl;

            return success;
        }
        catch(const std::exception& e)
        {
            printTestResult("Get History Operations", false, "Exception: " + std::string(e.what()));
            return false;
        }
    }

    bool test3_get()
    {
        printHeader("TEST 3: GET OPERATIONS");
        std::cout << "  Retrieving " << num_random_gets << " random specific values by timestamp" << std::endl;
        std::cout << "  Target: " << num_random_gets << " successful exact retrievals" << std::endl;
        printSeparator();

        int successful_gets = 0;

        for(int i = 0; i < num_random_gets; i++)
        {
            int idx = random_indices[i];
            std::string expected_value = values[idx];
            std::uint64_t timestamp = timestamps[idx];

            try
            {
                std::string retrieved_value = kvs.get(test_key, timestamp);
                if(retrieved_value == expected_value)
                {
                    successful_gets++;
                    std::cout << "  ✓ Get " << std::setw(2) << (i + 1) << "/" << num_random_gets << ": timestamp "
                              << std::setw(12) << timestamp << " -> " << retrieved_value << std::endl;
                }
                else
                {
                    std::cout << "  ✗ Get " << std::setw(2) << (i + 1) << "/" << num_random_gets << ": timestamp "
                              << std::setw(12) << timestamp << " -> expected '" << expected_value << "' but got '"
                              << retrieved_value << "'" << std::endl;
                }
            }
            catch(const std::exception& e)
            {
                std::cout << "  ✗ Get " << std::setw(2) << (i + 1) << "/" << num_random_gets << ": timestamp "
                          << std::setw(12) << timestamp << " failed with " << e.what() << std::endl;
            }
        }

        std::cout << std::endl;
        bool success = successful_gets == num_random_gets;
        printTestResult("Get Operations",
                        success,
                        std::to_string(successful_gets) + "/" + std::to_string(num_random_gets) + " successful");

        if(success)
        {
            std::cout << "  ✓ All exact timestamp retrievals successful" << std::endl;
        }
        else
        {
            std::cout << "  ⚠️  Some exact retrievals failed - this may indicate timing precision issues" << std::endl;
        }

        return success;
    }

    bool test4_get_range()
    {
        printHeader("TEST 4: GET RANGE OPERATIONS");
        std::cout << "  Retrieving events within specific time ranges" << std::endl;
        std::cout << "  Target: Successful range retrievals with correct event counts" << std::endl;
        printSeparator();

        // Use history events from Test 2, which are guaranteed to be available
        if(history_events.size() < 10)
        {
            std::cout << "  ⚠️  Skipping Test 4 (Get Range) - insufficient data (need at least 10 events in history)"
                      << std::endl;
            return false;
        }

        int successful_ranges = 0;
        const int num_range_tests = 5;
        const int min_successful = 3; // Test passes if at least 3 out of 5 ranges succeed

        // Test different ranges using actual available events
        // Use sequential ranges from different parts of the history for more predictable testing
        for(int i = 0; i < num_range_tests; i++)
        {
            // Use sequential ranges from different parts of the history
            // This is more predictable than random ranges
            int history_size = (int)history_events.size();
            int segment_size = history_size / num_range_tests;
            int start_idx = i * segment_size;
            int end_idx = std::min(start_idx + std::max(5, segment_size / 2), history_size - 1);

            // Ensure we have a valid range with at least 2 events
            if(end_idx <= start_idx)
            {
                end_idx = std::min(start_idx + 5, history_size - 1);
            }

            std::uint64_t start_ts = history_events[start_idx].timestamp;
            std::uint64_t end_ts = history_events[end_idx].timestamp + 1; // +1 to make it inclusive of the last event

            // Ensure valid range (end must be greater than start)
            if(end_ts <= start_ts)
            {
                // If timestamps are the same or out of order, use a small offset
                end_ts = start_ts + 1000; // Add a small offset
            }

            try
            {
                auto range_events = kvs.get_range(test_key, start_ts, end_ts);

                // Count how many events should be in this range from the available history
                // Note: We iterate over ALL events, not just [start_idx, end_idx], because
                // if end_ts was adjusted (line 278), the actual query range may include events
                // outside the original slice
                int expected_count = 0;
                for(std::size_t j = 0; j < history_events.size(); j++)
                {
                    if(history_events[j].timestamp >= start_ts && history_events[j].timestamp < end_ts)
                    {
                        expected_count++;
                    }
                }

                // Verify we got at least the expected number of events
                // (may be more due to data propagation from previous tests)
                bool count_check = range_events.size() >= static_cast<size_t>(expected_count);

                // Verify that all retrieved events are within the range
                bool all_in_range = true;
                for(const auto& event: range_events)
                {
                    if(event.timestamp < start_ts || event.timestamp >= end_ts)
                    {
                        all_in_range = false;
                        std::cout << "    ⚠️  Event with timestamp " << event.timestamp << " is outside the range ["
                                  << start_ts << ", " << end_ts << ")" << std::endl;
                    }
                }

                // Range is successful only if both checks pass
                bool range_success = count_check && all_in_range;

                if(range_success)
                {
                    successful_ranges++;
                    std::cout << "  ✓ Range " << std::setw(2) << (i + 1) << "/" << num_range_tests << ": ["
                              << std::setw(12) << start_ts << ", " << std::setw(12) << end_ts << ") -> "
                              << range_events.size() << " events (expected at least " << expected_count << ")"
                              << std::endl;
                }
                else
                {
                    std::cout << "  ✗ Range " << std::setw(2) << (i + 1) << "/" << num_range_tests << ": ["
                              << std::setw(12) << start_ts << ", " << std::setw(12) << end_ts << ") -> "
                              << range_events.size() << " events (expected at least " << expected_count << ")"
                              << std::endl;
                }
            }
            catch(const std::exception& e)
            {
                std::cout << "  ✗ Range " << std::setw(2) << (i + 1) << "/" << num_range_tests << ": [" << std::setw(12)
                          << start_ts << ", " << std::setw(12) << end_ts << ") failed with " << e.what() << std::endl;
            }
        }

        std::cout << std::endl;
        bool success = successful_ranges >= min_successful;
        printTestResult("Get Range Operations",
                        success,
                        std::to_string(successful_ranges) + "/" + std::to_string(num_range_tests) +
                                " successful (need at least " + std::to_string(min_successful) + ")");

        if(success)
        {
            std::cout << "  ✓ Sufficient range retrievals successful" << std::endl;
        }
        else
        {
            std::cout << "  ⚠️  Some range retrievals failed - this may indicate timing or data propagation issues"
                      << std::endl;
        }

        return success;
    }

    bool test5_get_earliest()
    {
        printHeader("TEST 5: GET EARLIEST OPERATIONS");
        std::cout << "  Retrieving the earliest event for a key" << std::endl;
        std::cout << "  Target: Successful earliest event retrieval with correct timestamp" << std::endl;
        printSeparator();

        // Use history events from Test 2, which are guaranteed to be available
        if(history_events.size() < 1)
        {
            std::cout << "  ⚠️  Skipping Test 5 (Get Earliest) - insufficient data (need at least 1 event in history)"
                      << std::endl;
            return false;
        }

        try
        {
            auto earliest_opt = kvs.get_earliest(test_key);

            if(!earliest_opt.has_value())
            {
                std::cout << "  ✗ Get Earliest: No events found for key '" << test_key << "'" << std::endl;
                printTestResult("Get Earliest Operations", false, "No events found");
                return false;
            }

            auto earliest = earliest_opt.value();

            // Find the actual earliest event from history_events for comparison
            auto expected_earliest = std::min_element(history_events.begin(),
                                                      history_events.end(),
                                                      [](const chronokvs::EventData& a, const chronokvs::EventData& b)
                                                      { return a.timestamp < b.timestamp; });

            bool timestamp_match = earliest.timestamp == expected_earliest->timestamp;
            bool value_match = earliest.value == expected_earliest->value;

            if(timestamp_match && value_match)
            {
                std::cout << "  ✓ Get Earliest: timestamp " << std::setw(12) << earliest.timestamp << " -> "
                          << earliest.value << std::endl;
                std::cout << "    Matches expected earliest event from history" << std::endl;
                printTestResult("Get Earliest Operations", true, "Earliest event retrieved correctly");
                return true;
            }
            else
            {
                std::cout << "  ✗ Get Earliest: timestamp " << std::setw(12) << earliest.timestamp << " -> "
                          << earliest.value << std::endl;
                if(!timestamp_match)
                {
                    std::cout << "    ⚠️  Timestamp mismatch: expected " << expected_earliest->timestamp << ", got "
                              << earliest.timestamp << std::endl;
                }
                if(!value_match)
                {
                    std::cout << "    ⚠️  Value mismatch: expected '" << expected_earliest->value << "', got '"
                              << earliest.value << "'" << std::endl;
                }
                printTestResult("Get Earliest Operations", false, "Earliest event does not match expected");
                return false;
            }
        }
        catch(const std::exception& e)
        {
            std::cout << "  ✗ Get Earliest: failed with " << e.what() << std::endl;
            printTestResult("Get Earliest Operations", false, "Exception: " + std::string(e.what()));
            return false;
        }
    }

    bool test6_get_latest()
    {
        printHeader("TEST 6: GET LATEST OPERATIONS");
        std::cout << "  Retrieving the latest event for a key" << std::endl;
        std::cout << "  Target: Successful latest event retrieval with correct timestamp" << std::endl;
        printSeparator();

        // Use history events from Test 2, which are guaranteed to be available
        if(history_events.size() < 1)
        {
            std::cout << "  ⚠️  Skipping Test 6 (Get Latest) - insufficient data (need at least 1 event in history)"
                      << std::endl;
            return false;
        }

        try
        {
            auto latest_opt = kvs.get_latest(test_key);

            if(!latest_opt.has_value())
            {
                std::cout << "  ✗ Get Latest: No events found for key '" << test_key << "'" << std::endl;
                printTestResult("Get Latest Operations", false, "No events found");
                return false;
            }

            auto latest = latest_opt.value();

            // Find the actual latest event from history_events for comparison
            auto expected_latest = std::max_element(history_events.begin(),
                                                    history_events.end(),
                                                    [](const chronokvs::EventData& a, const chronokvs::EventData& b)
                                                    { return a.timestamp < b.timestamp; });

            bool timestamp_match = latest.timestamp == expected_latest->timestamp;
            bool value_match = latest.value == expected_latest->value;

            if(timestamp_match && value_match)
            {
                std::cout << "  ✓ Get Latest: timestamp " << std::setw(12) << latest.timestamp << " -> " << latest.value
                          << std::endl;
                std::cout << "    Matches expected latest event from history" << std::endl;
                printTestResult("Get Latest Operations", true, "Latest event retrieved correctly");
                return true;
            }
            else
            {
                std::cout << "  ✗ Get Latest: timestamp " << std::setw(12) << latest.timestamp << " -> " << latest.value
                          << std::endl;
                if(!timestamp_match)
                {
                    std::cout << "    ⚠️  Timestamp mismatch: expected " << expected_latest->timestamp << ", got "
                              << latest.timestamp << std::endl;
                }
                if(!value_match)
                {
                    std::cout << "    ⚠️  Value mismatch: expected '" << expected_latest->value << "', got '"
                              << latest.value << "'" << std::endl;
                }
                printTestResult("Get Latest Operations", false, "Latest event does not match expected");
                return false;
            }
        }
        catch(const std::exception& e)
        {
            std::cout << "  ✗ Get Latest: failed with " << e.what() << std::endl;
            printTestResult("Get Latest Operations", false, "Exception: " + std::string(e.what()));
            return false;
        }
    }

    bool runAllTests()
    {
        // Main header
        std::cout << "\n" << std::string(80, '=') << std::endl;
        std::cout << "                    CHRONOKVS API INTEGRATION TEST" << std::endl;
        std::cout << "                    ==============================" << std::endl;
        std::cout << "  Purpose: Verify ChronoKVS API functionality for GitHub Actions" << std::endl;
        std::cout << "  Tests:   Put Operations, Data Propagation, Get History, Get Operations, Get Range Operations, "
                     "Get Earliest Operations, Get Latest Operations"
                  << std::endl;
        std::cout << std::string(80, '=') << std::endl;

        bool test1_result = false;
        bool test2_result = false;
        bool test3_result = false;
        bool test4_result = false;
        bool test5_result = false;
        bool test6_result = false;

        // Test 1: Put
        test1_result = test1_put();

        // Wait for data propagation (only if Test 1 passed)
        if(test1_result)
        {
            waitForDataPropagation();
        }
        else
        {
            printHeader("DATA PROPAGATION WAIT");
            std::cout << "  ⚠️  Skipping data propagation wait due to Test 1 failure" << std::endl;
            std::cout << "  Subsequent tests may not have data to work with" << std::endl;
        }

        // Test 2: Get History
        test2_result = test2_get_history();

        // Test 3: Get (only if we have data from previous tests)
        if(test1_result && timestamps.size() > 0)
        {
            test3_result = test3_get();
        }
        else
        {
            printHeader("TEST 3: GET OPERATIONS");
            std::cout << "  ⚠️  Skipping Test 3 (Get) - no data available from Test 1" << std::endl;
            std::cout << "  This test requires successful put operations to proceed" << std::endl;
            test3_result = false;
        }

        // Test 4: Get Range (only if we have history data from Test 2, regardless of Test 2 pass/fail)
        // Test 2 may have "failed" because it didn't get all 1000 events, but we still got some history
        if(history_events.size() >= 10)
        {
            test4_result = test4_get_range();
        }
        else
        {
            printHeader("TEST 4: GET RANGE OPERATIONS");
            std::cout << "  ⚠️  Skipping Test 4 (Get Range) - insufficient history data from Test 2" << std::endl;
            std::cout << "  This test requires at least 10 events in history to proceed (got " << history_events.size()
                      << ")" << std::endl;
            test4_result = false;
        }

        // Test 5: Get Earliest (only if we have history data from Test 2, regardless of Test 2 pass/fail)
        if(history_events.size() >= 1)
        {
            test5_result = test5_get_earliest();
        }
        else
        {
            printHeader("TEST 5: GET EARLIEST OPERATIONS");
            std::cout << "  ⚠️  Skipping Test 5 (Get Earliest) - insufficient history data from Test 2" << std::endl;
            std::cout << "  This test requires at least 1 event in history to proceed (got " << history_events.size()
                      << ")" << std::endl;
            test5_result = false;
        }

        // Test 6: Get Latest (only if we have history data from Test 2, regardless of Test 2 pass/fail)
        if(history_events.size() >= 1)
        {
            test6_result = test6_get_latest();
        }
        else
        {
            printHeader("TEST 6: GET LATEST OPERATIONS");
            std::cout << "  ⚠️  Skipping Test 6 (Get Latest) - insufficient history data from Test 2" << std::endl;
            std::cout << "  This test requires at least 1 event in history to proceed (got " << history_events.size()
                      << ")" << std::endl;
            test6_result = false;
        }

        // Final summary
        printHeader("FINAL TEST RESULTS");
        std::cout << "  Test 1 (Put):        " << (test1_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        std::cout << "  Test 2 (Get History): " << (test2_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        std::cout << "  Test 3 (Get):        " << (test3_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        std::cout << "  Test 4 (Get Range):  " << (test4_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        std::cout << "  Test 5 (Get Earliest): " << (test5_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        std::cout << "  Test 6 (Get Latest): " << (test6_result ? "✅ PASSED" : "❌ FAILED") << std::endl;
        printSeparator();

        int passed_tests = (test1_result ? 1 : 0) + (test2_result ? 1 : 0) + (test3_result ? 1 : 0) +
                           (test4_result ? 1 : 0) + (test5_result ? 1 : 0) + (test6_result ? 1 : 0);
        std::cout << "  Overall: " << passed_tests << "/6 tests passed" << std::endl;

        if(passed_tests == 6)
        {
            std::cout << "\n  🎉 ALL CHRONOKVS API TESTS PASSED! 🎉" << std::endl;
            std::cout << "  The ChronoKVS integration is working correctly and ready for GitHub Actions." << std::endl;
        }
        else if(passed_tests > 0)
        {
            std::cout << "\n  ⚠️  PARTIAL SUCCESS: " << passed_tests << "/6 tests passed" << std::endl;
            std::cout << "  Some ChronoKVS functionality is working, but there may be issues to investigate."
                      << std::endl;
        }
        else
        {
            std::cout << "\n  ❌ ALL TESTS FAILED" << std::endl;
            std::cout << "  ChronoKVS integration has critical issues that need to be resolved." << std::endl;
        }

        std::cout << std::string(80, '=') << std::endl;

        return passed_tests == 6;
    }
};

int main()
{
    try
    {
        ChronoKVSTest test;
        bool success = test.runAllTests();
        return success ? 0 : 1;
    }
    catch(const std::exception& e)
    {
        std::cerr << "\n" << std::string(80, '!') << std::endl;
        std::cerr << "  CRITICAL ERROR: Test failed with exception" << std::endl;
        std::cerr << "  " << e.what() << std::endl;
        std::cerr << "  This indicates a serious issue with the ChronoKVS integration." << std::endl;
        std::cerr << std::string(80, '!') << std::endl;
        return 1;
    }
    catch(...)
    {
        std::cerr << "\n" << std::string(80, '!') << std::endl;
        std::cerr << "  CRITICAL ERROR: Test failed with unknown exception" << std::endl;
        std::cerr << "  This indicates an unexpected error in the ChronoKVS integration." << std::endl;
        std::cerr << std::string(80, '!') << std::endl;
        return 1;
    }
}
