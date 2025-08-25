//
// Created by kfeng on 1/14/25.
//
#include <iostream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <HDF5ArchiveReadingAgent.h>
#include <chrono_monitor.h>

namespace fs = std::filesystem;

void createTestFile(const std::string& path, const std::string& content = "test content")
{
    std::ofstream file(path);
    if (file.is_open()) {
        file << content;
        file.close();
        std::cout << "Created test file: " << path << std::endl;
    } else {
        std::cerr << "Failed to create test file: " << path << std::endl;
    }
}

void deleteTestFile(const std::string& path)
{
    if (fs::remove(path)) {
        std::cout << "Deleted test file: " << path << std::endl;
    } else {
        std::cerr << "Failed to delete test file: " << path << std::endl;
    }
}

void testInotifyMode(const std::string& test_dir)
{
    std::cout << "\n=== Testing Inotify Mode ===" << std::endl;
    
    // Create test directory
    fs::create_directories(test_dir);
    
    // Initialize agent with inotify mode
    chronolog::HDF5ArchiveReadingAgent agent(test_dir, false); // use_polling = false
    
    if (agent.initialize() != 0) {
        std::cerr << "Failed to initialize agent in inotify mode" << std::endl;
        return;
    }
    
    std::cout << "Agent initialized in inotify mode" << std::endl;
    
    // Wait a bit for monitoring to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Create a test HDF5 file
    std::string test_file = test_dir + "/chronicle_0_0.story_0_0.1736806500.vlen.h5";
    createTestFile(test_file);
    
    // Wait for the monitoring to detect the file
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Clean up
    deleteTestFile(test_file);
    agent.shutdown();
    
    std::cout << "Inotify mode test completed" << std::endl;
}

void testPollingMode(const std::string& test_dir)
{
    std::cout << "\n=== Testing Polling Mode ===" << std::endl;
    
    // Create test directory
    fs::create_directories(test_dir);
    
    // Initialize agent with polling mode
    chronolog::HDF5ArchiveReadingAgent agent(test_dir, true, std::chrono::milliseconds(500)); // use_polling = true, 500ms interval
    
    if (agent.initialize() != 0) {
        std::cerr << "Failed to initialize agent in polling mode" << std::endl;
        return;
    }
    
    std::cout << "Agent initialized in polling mode" << std::endl;
    
    // Wait a bit for monitoring to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Create a test HDF5 file
    std::string test_file = test_dir + "/chronicle_0_0.story_0_0.1736806500.vlen.h5";
    createTestFile(test_file);
    
    // Wait for the polling to detect the file (should be within 500ms + buffer)
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    // Clean up
    deleteTestFile(test_file);
    agent.shutdown();
    
    std::cout << "Polling mode test completed" << std::endl;
}

int main(int argc, char* argv[])
{
    // Initialize logging
    chronolog::chrono_monitor::initialize("console", "", spdlog::level::debug, "HDF5ArchiveReadingAgentTest", 0, 0, spdlog::level::debug);
    
    std::string test_dir = "/tmp/chronolog_test_monitoring";
    
    std::cout << "Testing HDF5ArchiveReadingAgent with both inotify and polling modes" << std::endl;
    std::cout << "Test directory: " << test_dir << std::endl;
    
    try {
        testInotifyMode(test_dir);
        testPollingMode(test_dir);
        
        // Clean up test directory
        fs::remove_all(test_dir);
        
        std::cout << "\nAll tests completed successfully!" << std::endl;
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
