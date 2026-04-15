---
sidebar_position: 4
title: "Examples"
---

# C++ Client Examples

## Basic Example

This example demonstrates the full lifecycle of a ChronoLog session: connecting to ChronoVisor, creating a chronicle, acquiring a story, logging events, replaying the events, and cleaning up. It corresponds to the `client_lib_basic_example.cpp` file in the source tree.

```cpp
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>
#include <cmd_arg_parse.h>

int main(int argc, char** argv)
{
    // ── 1. Load configuration ────────────────────────────────────────────────
    // parse_conf_path_arg reads the -conf <path> command-line option.
    // If no path is supplied the ClientConfiguration defaults are used:
    //   portal → 127.0.0.1:5555, query → 127.0.0.1:5557
    std::string conf_file_path = parse_conf_path_arg(argc, argv);
    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty())
    {
        if(!confManager.load_from_file(conf_file_path))
            std::cerr << "[ClientExample] Failed to load config. Using defaults.\n";
        else
            std::cout << "[ClientExample] Config loaded from '" << conf_file_path << "'.\n";
    }
    else
    {
        std::cout << "[ClientExample] No config file provided. Using defaults.\n";
    }
    confManager.log_configuration(std::cout);

    // ── 2. Initialise structured logging ────────────────────────────────────
    int result = chronolog::chrono_monitor::initialize(
        confManager.LOG_CONF.LOGTYPE,
        confManager.LOG_CONF.LOGFILE,
        confManager.LOG_CONF.LOGLEVEL,
        confManager.LOG_CONF.LOGNAME,
        confManager.LOG_CONF.LOGFILESIZE,
        confManager.LOG_CONF.LOGFILENUM,
        confManager.LOG_CONF.FLUSHLEVEL);
    if(result == 1) return EXIT_FAILURE;

    // ── 3. Build service configurations ─────────────────────────────────────
    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF  = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP          = confManager.PORTAL_CONF.IP;
    portalConf.PORT        = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    chronolog::ClientQueryServiceConf queryConf;
    queryConf.PROTO_CONF  = confManager.QUERY_CONF.PROTO_CONF;
    queryConf.IP          = confManager.QUERY_CONF.IP;
    queryConf.PORT        = confManager.QUERY_CONF.PORT;
    queryConf.PROVIDER_ID = confManager.QUERY_CONF.PROVIDER_ID;

    // ── 4. Create client and connect ─────────────────────────────────────────
    // Passing both portalConf and queryConf enables WRITER + READER mode,
    // which allows both log_event and ReplayStory to be used.
    chronolog::Client client(portalConf, queryConf);

    int ret = client.Connect();
    std::cout << "[ClientExample] Connect: " << chronolog::to_string_client(ret) << "\n";

    // ── 5. Create a chronicle ────────────────────────────────────────────────
    std::string chronicle_name = "MyChronicle";
    int flags = 0;
    ret = client.CreateChronicle(chronicle_name, {}, flags);
    std::cout << "[ClientExample] CreateChronicle: " << chronolog::to_string_client(ret) << "\n";

    // ── 6. Acquire a story ───────────────────────────────────────────────────
    std::string story_name = "MyStory";
    auto [acquire_ret, story_handle] = client.AcquireStory(chronicle_name, story_name, {}, flags);
    std::cout << "[ClientExample] AcquireStory: " << chronolog::to_string_client(acquire_ret) << "\n";

    // ── 7. Log events ────────────────────────────────────────────────────────
    // log_event returns the uint64_t timestamp assigned to the event.
    story_handle->log_event("Event 1");
    story_handle->log_event("Event 2");
    story_handle->log_event("Event 3");

    // ── 8. Wait for persistence ──────────────────────────────────────────────
    // Background processes in ChronoLog need time to flush and index events
    // before they become visible to ReplayStory. The required duration depends
    // on system configuration and load.
    //
    // NOTE: This is a temporary workaround. Future versions may provide an
    // explicit flush/sync mechanism that eliminates this fixed delay.
    sleep(120);

    // ── 9. Replay events ─────────────────────────────────────────────────────
    // Use a wide time range to capture all events regardless of exact timing.
    uint64_t start_time = 1;
    uint64_t end_time   = 2000000000000000000ULL;

    std::vector<chronolog::Event> events;
    ret = client.ReplayStory(chronicle_name, story_name, start_time, end_time, events);
    std::cout << "[ClientExample] ReplayStory: " << chronolog::to_string_client(ret) << "\n";

    if(ret == chronolog::CL_SUCCESS)
    {
        std::cout << "[ClientExample] Replayed " << events.size() << " event(s):\n";
        for(const auto& ev : events)
            std::cout << ev.to_string() << "\n";
    }

    // ── 10. Clean up ─────────────────────────────────────────────────────────
    ret = client.ReleaseStory(chronicle_name, story_name);
    std::cout << "[ClientExample] ReleaseStory: " << chronolog::to_string_client(ret) << "\n";

    ret = client.DestroyStory(chronicle_name, story_name);
    std::cout << "[ClientExample] DestroyStory: " << chronolog::to_string_client(ret) << "\n";

    ret = client.DestroyChronicle(chronicle_name);
    std::cout << "[ClientExample] DestroyChronicle: " << chronolog::to_string_client(ret) << "\n";

    ret = client.Disconnect();
    std::cout << "[ClientExample] Disconnect: " << chronolog::to_string_client(ret) << "\n";

    return 0;
}
```

### Key Steps Explained

| Step | What it does |
|------|-------------|
| **Load configuration** | `ClientConfiguration::load_from_file` parses a JSON config file and populates the portal and query service structs. If no file is provided the defaults (`127.0.0.1:5555` / `127.0.0.1:5557`) are used. |
| **Connect** | Opens a session with ChronoVisor over the configured transport (`ofi+sockets` by default). |
| **CreateChronicle** | Registers a top-level namespace. Returns `CL_ERR_CHRONICLE_EXISTS` if one already exists with that name, which is safe to ignore if you just want to ensure it exists. |
| **AcquireStory** | Opens (and creates if necessary) a story within the chronicle. The returned `StoryHandle*` is used for all subsequent writes. |
| **log_event** | Appends an event record to the story. The call is non-blocking; events are buffered and flushed in the background. |
| **sleep(120)** | Waits for background persistence to complete before issuing a replay. See the note in the code above. |
| **ReplayStory** | Fetches all persisted events in the given time range. Requires the client to have been constructed with a `ClientQueryServiceConf` (READER mode). |
| **ReleaseStory** | Flushes buffered events and releases the story handle. Must be called before `DestroyStory`. |
| **DestroyStory / DestroyChronicle** | Permanently removes the story and chronicle. Both fail with `CL_ERR_ACQUIRED` if the resource is still held by any client. |

### Expected Console Output

```
[ClientExample] No config file provided. Using defaults.
...
[ClientExample] Connect: CL_SUCCESS
[ClientExample] CreateChronicle: CL_SUCCESS
[ClientExample] AcquireStory: CL_SUCCESS
[ClientExample] ReplayStory: CL_SUCCESS
[ClientExample] Replayed 3 event(s):
{Event:1234567890:42:0:Event 1}
{Event:1234567891:42:1:Event 2}
{Event:1234567892:42:2:Event 3}
[ClientExample] ReleaseStory: CL_SUCCESS
[ClientExample] DestroyStory: CL_SUCCESS
[ClientExample] DestroyChronicle: CL_SUCCESS
[ClientExample] Disconnect: CL_SUCCESS
```

Actual timestamps and client IDs will differ on each run.
