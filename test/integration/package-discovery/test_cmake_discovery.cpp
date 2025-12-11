/**
 * @file test_cmake_discovery.cpp
 * @brief Test program to verify CMake find_package(Chronolog) discovery works correctly.
 * 
 * This test verifies that:
 * 1. find_package(Chronolog) can locate the installed package
 * 2. The Chronolog::chronolog_client target is available
 * 3. Headers can be included and linked successfully
 * 4. The library is actually linked (not just headers included)
 */

#include <iostream>
#include <chronolog_client.h>
#include <ClientConfiguration.h>

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

    if(test_event.index() != 0)
    {
        std::cerr << "ERROR: Event index mismatch" << std::endl;
        return 1;
    }

    if(test_event.log_record() != "test_event")
    {
        std::cerr << "ERROR: Event log_record mismatch" << std::endl;
        return 1;
    }

    // Test that we can actually link to libchronolog_client.so
    // by creating a Client object. This forces the linker to link the library.
    // We use default configuration and don't connect, so this is safe for discovery testing.
    chronolog::ClientPortalServiceConf portal_conf;
    chronolog::Client client(portal_conf);
    
    // Verify the client object was created (destructor will be called automatically)
    // This ensures the library symbols are actually linked, not just header-included

    std::cout << "SUCCESS: CMake discovery test passed!" << std::endl;
    std::cout << "  - find_package(Chronolog) works correctly" << std::endl;
    std::cout << "  - Chronolog::chronolog_client target links successfully" << std::endl;
    std::cout << "  - Headers are accessible and functional" << std::endl;
    std::cout << "  - Library is actually linked (libchronolog_client.so)" << std::endl;

    return 0;
}
