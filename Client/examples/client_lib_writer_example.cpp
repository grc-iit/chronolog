#include <chronolog_client.h>
#include <common.h>
#include <cassert>

int main() {
    // Configure the client connection
    chronolog::ClientPortalServiceConf portalConf("ofi+sockets", "127.0.0.1", 5555, 55);

    // Create a ChronoLog client
    chronolog::Client client(portalConf);

    // Connect to ChronoVisor
    int ret = client.Connect();
    assert(ret == chronolog::CL_SUCCESS);

    // Create a chronicle
    std::string chronicle_name = "MyChronicle";
    std::map<std::string, std::string> chronicle_attrs;
    int flags = 0;
    ret = client.CreateChronicle(chronicle_name, chronicle_attrs, flags);
    assert(ret == chronolog::CL_SUCCESS);

    // Acquire a story
    std::string story_name = "MyStory";
    std::map<std::string, std::string> story_attrs;
    auto acquire_result = client.AcquireStory(chronicle_name, story_name, story_attrs, flags);
    assert(acquire_result.first == chronolog::CL_SUCCESS);
    auto story_handle = acquire_result.second;

    // Log a few events to the story
    story_handle->log_event("Event 1");
    story_handle->log_event("Event 2");
    story_handle->log_event("Event 3");

    // Release the story
    ret = client.ReleaseStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS);

    // Destroy the story
    ret = client.DestroyStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS);

    // Destroy the chronicle
    ret = client.DestroyChronicle(chronicle_name);
    assert(ret == chronolog::CL_SUCCESS);

    // Disconnect from ChronoVisor
    ret = client.Disconnect();
    assert(ret == chronolog::CL_SUCCESS);

    return 0;
}