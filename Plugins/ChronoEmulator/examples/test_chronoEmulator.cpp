#include "mock_chronolog.h"
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>

void TestWithoutConnection()
{
    MockChronolog chronolog;
    std::unordered_map <std::string, std::string> attrs = {{"creator", "John Doe"}};
    int flags = 0;

    // Attempt to create a chronicle without a connection
    assert(chronolog.CreateChronicle("TestChronicle", attrs, flags) != 0);

    // Attempt to destroy a chronicle without a connection
    assert(chronolog.DestroyChronicle("TestChronicle") != 0);

    std::cout << "TestWithoutConnection: All tests passed!" << std::endl;
}

void TestWithConnection()
{
    MockChronolog chronolog;
    std::unordered_map <std::string, std::string> attrs = {{"creator", "John Doe"}};
    int flags = 0;

    // Step 1: Connect
    assert(chronolog.Connect() == 0);

    // Step 2: Create a Chronicle
    assert(chronolog.CreateChronicle("TestChronicle", attrs, flags) == 0);

    // Step 3: Destroy the Chronicle
    assert(chronolog.DestroyChronicle("TestChronicle") == 0);

    // Step 4: Disconnect
    assert(chronolog.Disconnect() == 0);

    std::cout << "TestWithConnection: All tests passed!" << std::endl;
}

void TestStories()
{
    MockChronolog chronolog;
    int flags = 0; // Placeholder for flags, adjust as necessary for your implementation

    // Connect to MockChronolog
    assert(chronolog.Connect() == 0);

    // Create Chronicle for further testing
    assert(chronolog.CreateChronicle("TestChronicle", {{  "creator", "John Doe"}
                                                       , {"purpose", "testing"}}, flags) == 0);

    // Test 3: Acquire, release, and destroy a Story
    std::unordered_map <std::string, std::string> storyAttrs = {{  "author" , "Jane Doe"}
                                                                , {"summary", "example story"}};
    auto [acquireStatus, storyHandle] = chronolog.AcquireStory("TestChronicle", "Story1", storyAttrs, flags);
    assert(acquireStatus == 0 && storyHandle != nullptr); // Check if story was successfully acquired

    assert(chronolog.ReleaseStory("TestChronicle", "Story1") == 0); // Test releasing the story
    assert(chronolog.DestroyStory("TestChronicle", "Story1") == 0); // Test destroying the story

    // Clean up: Destroy Chronicle and Disconnect
    assert(chronolog.DestroyChronicle("TestChronicle") == 0);
    assert(chronolog.Disconnect() == 0);

    std::cout << "All tests passed!" << std::endl;
}

void TestRecordAndReplay()
{
    MockChronolog chronolog;
    int flags = 0; // Placeholder for flags, adjust as necessary for your implementation

    // Connect to MockChronolog
    assert(chronolog.Connect() == 0);

    // Create Chronicle for further testing
    assert(chronolog.CreateChronicle("TestChronicle", {{  "creator", "John Doe"}
                                                       , {"purpose", "testing"}}, flags) == 0);

    // Acquire, release, and destroy a Story
    std::unordered_map <std::string, std::string> storyAttrs = {{  "author" , "Jane Doe"}
                                                                , {"summary", "example story"}};
    auto [acquireStatus, storyHandle] = chronolog.AcquireStory("TestChronicle", "Story1", storyAttrs, flags);
    assert(acquireStatus == 0 && storyHandle != nullptr); // Check if story was successfully acquired

    // Record some events with slight delays to ensure different timestamps
    auto timestamp1 = storyHandle->record("Event 1");
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Ensure a different timestamp
    auto timestamp2 = storyHandle->record("Event 2");
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Ensure a different timestamp
    auto timestamp3 = storyHandle->record("Event 3");
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Ensure a different timestamp

    // Test if replaying by a specific timestamp retrieves the correct events
    auto events1 = storyHandle->replay(timestamp1);
    assert(events1.size() == 1 && events1[0] == "Event 1");
    auto events2 = storyHandle->replay(timestamp2);
    assert(events2.size() == 1 && events2[0] == "Event 2");
    auto events3 = storyHandle->replay(timestamp3);
    assert(events3.size() == 1 && events3[0] == "Event 3");

    // Optionally, verify that querying for a non-existing timestamp returns an empty vector
    auto eventsNonExistent = storyHandle->replay(timestamp3 + 100); // Assuming no event has this timestamp
    assert(eventsNonExistent.empty());

    assert(chronolog.ReleaseStory("TestChronicle", "Story1") == 0); // Test releasing the story

    // Record some events with slight delays to ensure different timestamps
    auto timestamp4 = storyHandle->record("Event 4");
    assert(timestamp4 == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Ensure a different timestamp
    auto timestamp5 = storyHandle->record("Event 5");
    assert(timestamp5 == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Ensure a different timestamp
    auto timestamp6 = storyHandle->record("Event 6");
    assert(timestamp6 == 1);

    // Test if replaying by a specific timestamp retrieves the correct events
    auto events4 = storyHandle->replay(timestamp4);
    assert(events4.empty());
    auto events5 = storyHandle->replay(timestamp5);
    assert(events5.empty());
    auto events6 = storyHandle->replay(timestamp6);
    assert(events6.empty());

    assert(chronolog.DestroyStory("TestChronicle", "Story1") == 0); // Test destroying the story

    // Record some events with slight delays to ensure different timestamps
    auto timestamp7 = storyHandle->record("Event 7");
    assert(timestamp7 == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Ensure a different timestamp
    auto timestamp8 = storyHandle->record("Event 8");
    assert(timestamp8 == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Ensure a different timestamp
    auto timestamp9 = storyHandle->record("Event 9");
    assert(timestamp9 == 1);

    // Test if replaying by a specific timestamp retrieves the correct events
    auto events7 = storyHandle->replay(timestamp7);
    assert(events7.empty());
    auto events8 = storyHandle->replay(timestamp8);
    assert(events8.empty());
    auto events9 = storyHandle->replay(timestamp9);
    assert(events9.empty());

    // Clean up: Destroy Chronicle and Disconnect
    assert(chronolog.DestroyChronicle("TestChronicle") == 0);
    assert(chronolog.Disconnect() == 0);

    std::cout << "All tests passed!" << std::endl;
}


int main()
{
    std::cout << "Running Test01: TestWithoutConnection" << std::endl;
    TestWithoutConnection();
    std::cout << "Running Test02: TestWithConnection" << std::endl;
    TestWithConnection();
    std::cout << "Running Test03: TestStories" << std::endl;
    TestStories();
    std::cout << "Running Test04: TestRecordAndReplay" << std::endl;
    TestRecordAndReplay();
    return 0;
}

