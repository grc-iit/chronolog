#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>
#include <cmd_arg_parse.h>

// This example demonstrates the tail-read API: StoryHandle::playback(n, events)
// returns the most recent `n` events of a story directly from the keepers'
// in-memory tail (sealed-but-not-yet-archived chunks), without going through
// the player/archive path used by Client::ReplayStory().
//
// Two things to know about playback():
//   1. It is a writer-mode API. It talks to the keeper recording service, so a
//      Client constructed with only the portal configuration (no query service
//      configuration) can use it.
//   2. Events become visible in the tail once their story chunk SEALS, i.e.
//      after chunk_duration + acceptance_window (keeper configuration; ~25-30s
//      with the default local deployment). The writer below therefore holds
//      the story acquired while it waits before playing back. Events still in
//      the active/open chunk are not returned.

int main(int argc, char** argv)
{
    // Load configuration
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty())
    {
        if(!confManager.load_from_file(conf_file_path))
        {
            std::cerr << "[TailReaderExample] Failed to load configuration file '" << conf_file_path
                      << "'. Using default values instead." << std::endl;
        }
        else
        {
            std::cout << "[TailReaderExample] Configuration file loaded successfully from '" << conf_file_path
                      << "'." << std::endl;
        }
    }
    else
    {
        std::cout << "[TailReaderExample] No configuration file provided. Using "
                     "default values."
                  << std::endl;
    }
    confManager.log_configuration(std::cout);

    // Initialize logging
    int result = chronolog::chrono_monitor::initialize(confManager.LOG_CONF.LOGTYPE,
                                                       confManager.LOG_CONF.LOGFILE,
                                                       confManager.LOG_CONF.LOGLEVEL,
                                                       confManager.LOG_CONF.LOGNAME,
                                                       confManager.LOG_CONF.LOGFILESIZE,
                                                       confManager.LOG_CONF.LOGFILENUM,
                                                       confManager.LOG_CONF.FLUSHLEVEL);
    if(result == 1)
    {
        return EXIT_FAILURE;
    }

    // Build portal config (writer mode is sufficient for playback)
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    LOG_INFO("[TailReaderExample] Starting ChronoLog Client Tail Reader Example");

    // Create a ChronoLog client
    chronolog::Client client(portalConf);

    // Connect to ChronoVisor
    int ret = client.Connect();
    std::cout << "[TailReaderExample] Connect returned: " << chronolog::to_string_client(ret) << "\n";

    // Create a chronicle and acquire a story
    std::string chronicle_name = "TailChronicle";
    std::string story_name = "TailStory";
    ret = client.CreateChronicle(chronicle_name);
    std::cout << "[TailReaderExample] CreateChronicle returned: " << chronolog::to_string_client(ret) << "\n";

    auto acquire_result = client.AcquireStory(chronicle_name, story_name);
    std::cout << "[TailReaderExample] AcquireStory returned: " << chronolog::to_string_client(acquire_result.first)
              << "\n";
    if(acquire_result.first != chronolog::CL_SUCCESS)
    {
        client.Disconnect();
        return EXIT_FAILURE;
    }
    auto story_handle = acquire_result.second;

    // Log some events
    const int event_count = 30;
    std::cout << "[TailReaderExample] Logging " << event_count << " events...\n";
    for(int i = 0; i < event_count; i++)
    {
        story_handle->log_event("Tail event #" + std::to_string(i));
    }

    // Keep the story acquired while the chunks seal into the keeper tail.
    // (chunk_duration + acceptance_window; ~30s for the default local deploy)
    const int settle_seconds = 40;
    std::cout << "[TailReaderExample] Holding the story acquired for " << settle_seconds
              << "s so the events seal into the keeper tail...\n";
    std::this_thread::sleep_for(std::chrono::seconds(settle_seconds));

    // Play back the most recent 10 events (expects events #20..#29, ascending)
    std::vector<chronolog::Event> events;
    ret = story_handle->playback(10, events);
    std::cout << "[TailReaderExample] playback(10) returned: " << chronolog::to_string_client(ret) << " with "
              << events.size() << " event(s):\n";
    for(size_t i = 0; i < events.size(); i++)
    {
        std::cout << "    [" << i << "] time=" << events[i].time() << " : " << events[i].log_record() << "\n";
    }

    // Asking for more than is available returns everything in the tail.
    events.clear();
    ret = story_handle->playback(1000, events);
    std::cout << "[TailReaderExample] playback(1000) returned: " << chronolog::to_string_client(ret) << " with "
              << events.size() << " event(s) (the whole tail)\n";

    // Release the story and disconnect
    ret = client.ReleaseStory(chronicle_name, story_name);
    std::cout << "[TailReaderExample] ReleaseStory returned: " << chronolog::to_string_client(ret) << "\n";

    ret = client.Disconnect();
    std::cout << "[TailReaderExample] Disconnect returned: " << chronolog::to_string_client(ret) << "\n";

    LOG_INFO("[TailReaderExample] Finished successfully");
    return 0;
}
