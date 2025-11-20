/**
 * @file test_cmake_discovery.cpp
 * @brief Test program to verify CMake find_package(Chronolog) discovery works correctly.
 * 
 * This test verifies that:
 * 1. find_package(Chronolog) can locate the installed package
 * 2. The Chronolog::chronolog_client target is available
 * 3. Headers can be included and linked successfully
 */

#include <iostream>
#include <chronolog_client.h>

int main()
{
    // Test that we can include the header
    std::cout << "Testing CMake discovery of Chronolog package..." << std::endl;

    // Test that we can use types from the namespace
    chronolog::Event test_event(12345, 67890, 0, "test_event");

    // Verify basic functionality
    if(test_event.time() != 12345)
    {
        std::cerr << "ERROR: Event time mismatch" << std::endl;
        return 1;
    }

    if(test_event.client_id() != 67890)
    {
        std::cerr << "ERROR: Event client_id mismatch" << std::endl;
        return 1;
    }

    if(test_event.log_record() != "test_event")
    {
        std::cerr << "ERROR: Event log_record mismatch" << std::endl;
        return 1;
    }

    std::cout << "SUCCESS: CMake discovery test passed!" << std::endl;
    std::cout << "  - find_package(Chronolog) works correctly" << std::endl;
    std::cout << "  - Chronolog::chronolog_client target links successfully" << std::endl;
    std::cout << "  - Headers are accessible and functional" << std::endl;

    return 0;
}
