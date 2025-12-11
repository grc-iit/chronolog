/**
 * @file test_pkgconfig_discovery.cpp
 * @brief Test program to verify pkg-config discovery works correctly.
 * 
 * This test verifies that:
 * 1. pkg-config can locate the installed chronolog.pc file
 * 2. Compiler and linker flags are correct
 * 3. Headers can be included and linked successfully
 * 4. The library is actually linked (not just headers included)
 */

#include <iostream>
#include <chronolog_client.h>
#include <ClientConfiguration.h>

int main()
{
    // Test that we can include the header
    std::cout << "Testing pkg-config discovery of Chronolog package..." << std::endl;

    // Test that we can use types from the namespace
    chronolog::Event test_event(54321, 98765, 1, "pkgconfig_test_event");

    // Verify basic functionality
    if(test_event.time() != 54321)
    {
        std::cerr << "ERROR: Event time mismatch" << std::endl;
        return 1;
    }

    if(test_event.client_id() != 98765)
    {
        std::cerr << "ERROR: Event client_id mismatch" << std::endl;
        return 1;
    }

    if(test_event.index() != 1)
    {
        std::cerr << "ERROR: Event index mismatch" << std::endl;
        return 1;
    }

    if(test_event.log_record() != "pkgconfig_test_event")
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

    std::cout << "SUCCESS: pkg-config discovery test passed!" << std::endl;
    std::cout << "  - pkg-config can locate chronolog.pc" << std::endl;
    std::cout << "  - Compiler flags (Cflags) are correct" << std::endl;
    std::cout << "  - Linker flags (Libs) are correct" << std::endl;
    std::cout << "  - Headers are accessible and functional" << std::endl;
    std::cout << "  - Library is actually linked (libchronolog_client.so)" << std::endl;

    return 0;
}
