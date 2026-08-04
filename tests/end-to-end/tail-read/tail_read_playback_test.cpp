// End-to-end tail-read (playback) test.
//
// Exercises the real two-phase tail-read RPC path — client
// StoryHandle::playback(N) → keeper KeeperTailStore across the story's assigned
// keepers — against a running ChronoLog deployment. This is coverage the
// header-only KeeperTailStore unit test cannot provide (it never touches the
// network or a live keeper).
//
// Like the data-integrity group, it assumes a stack is already running (the CI
// pipeline's Deploy ChronoLog step, or a developer's own deploy_local.sh
// --start). The test self-injects and self-cleans, so no ctest fixture is
// required. It writes N known events to a fresh, uniquely-named story, then
// polls playback(N) until the events become visible in the keeper tail and
// asserts the last-N come back correct.
//
// It works regardless of the keeper's live_tail_read setting: with the default
// (sealed-only) tail the events appear after the seal window
// (chunk_duration + acceptance_window); with live_tail_read on they appear
// within ~the ingestion tick. The poll loop tolerates both.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <getopt.h>
#include <unistd.h>

#include <ClientConfiguration.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>

namespace
{

// Resolve the client config: an explicit --config wins; otherwise
// $CHRONOLOG_INSTALL_DIR/conf, falling back to
// $HOME/chronolog-install/chronolog/conf (the deploy_local.sh default) — the
// same resolution the data-integrity fixture uses, since ctest does not export
// CHRONOLOG_INSTALL_DIR by default.
std::string resolve_config(std::string const& cli)
{
    if(!cli.empty())
        return cli;
    if(const char* install = std::getenv("CHRONOLOG_INSTALL_DIR"); install != nullptr && *install != '\0')
        return std::string(install) + "/conf/default-chrono-client-conf.json";
    if(const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
        return std::string(home) + "/chronolog-install/chronolog/conf/default-chrono-client-conf.json";
    return "";
}

bool connect_with_retry(chronolog::Client& client, int attempts, int sleep_ms)
{
    for(int i = 0; i < attempts; ++i)
    {
        if(client.Connect() == chronolog::CL_SUCCESS)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    std::string config_path;
    std::string chronicle = "tailread_e2e_chronicle";
    std::string story = "tailread_e2e_story";
    int event_count = 25;
    int max_wait_sec = 120;

    static option longopts[] = {{"config", required_argument, nullptr, 'c'},
                                {"chronicle", required_argument, nullptr, 'C'},
                                {"story", required_argument, nullptr, 's'},
                                {"events", required_argument, nullptr, 'n'},
                                {"max-wait", required_argument, nullptr, 'm'},
                                {"help", no_argument, nullptr, 'h'},
                                {nullptr, 0, nullptr, 0}};
    int opt;
    while((opt = getopt_long(argc, argv, "c:C:s:n:m:h", longopts, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                config_path = optarg;
                break;
            case 'C':
                chronicle = optarg;
                break;
            case 's':
                story = optarg;
                break;
            case 'n':
                event_count = std::atoi(optarg);
                break;
            case 'm':
                max_wait_sec = std::atoi(optarg);
                break;
            case 'h':
            default:
                std::cerr << "Usage: " << argv[0]
                          << " [--config <file>] [--chronicle <name>] [--story <name>]"
                             " [--events <n>] [--max-wait <sec>]\n";
                return 2;
        }
    }
    if(event_count <= 0)
    {
        std::cerr << "[tail-read-e2e] FAIL: --events must be positive\n";
        return 2;
    }

    // Make chronicle/story unique per run so repeated CI runs and other e2e
    // tests never collide on the same keeper tail.
    std::string suffix = "_" + std::to_string(static_cast<long>(getpid()));
    chronicle += suffix;
    story += suffix;

    config_path = resolve_config(config_path);
    chronolog::ClientConfiguration conf;
    if(config_path.empty() || !conf.load_from_file(config_path))
    {
        std::cerr << "[tail-read-e2e] FAIL: could not load client config from '" << config_path << "'\n";
        return 1;
    }

    chronolog::chrono_monitor::initialize("file",
                                          "tail_read_e2e.log",
                                          conf.LOG_CONF.LOGLEVEL,
                                          "TailReadE2E",
                                          conf.LOG_CONF.LOGFILESIZE,
                                          conf.LOG_CONF.LOGFILENUM,
                                          conf.LOG_CONF.FLUSHLEVEL);

    // Writer-mode client is sufficient: playback() talks to the keeper recording
    // service directly (no query-service configuration needed).
    chronolog::Client client(conf.PORTAL_CONF);
    if(!connect_with_retry(client, 60, 500))
    {
        std::cerr << "[tail-read-e2e] FAIL: Connect() failed after retries (is the stack up?)\n";
        return 1;
    }

    int rc = client.CreateChronicle(chronicle);
    if(rc != chronolog::CL_SUCCESS && rc != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        std::cerr << "[tail-read-e2e] FAIL: CreateChronicle rc=" << chronolog::to_string_client(rc) << "\n";
        client.Disconnect();
        return 1;
    }
    auto acq = client.AcquireStory(chronicle, story);
    if(acq.first != chronolog::CL_SUCCESS || acq.second == nullptr)
    {
        std::cerr << "[tail-read-e2e] FAIL: AcquireStory rc=" << chronolog::to_string_client(acq.first) << "\n";
        client.Disconnect();
        return 1;
    }
    chronolog::StoryHandle* handle = acq.second;

    // Log N known events; payload i == "tail-read-e2e-<i>".
    std::vector<std::string> expected;
    expected.reserve(event_count);
    for(int i = 0; i < event_count; ++i)
    {
        std::string payload = "tail-read-e2e-" + std::to_string(i);
        if(handle->log_event(payload) == 0)
        {
            std::cerr << "[tail-read-e2e] FAIL: log_event returned 0 at i=" << i << "\n";
            client.ReleaseStory(chronicle, story);
            client.Disconnect();
            return 1;
        }
        expected.push_back(payload);
    }
    std::cout << "[tail-read-e2e] logged " << event_count << " events to " << chronicle << "/" << story
              << "; polling playback() (up to " << max_wait_sec << "s) ...\n";

    // Poll playback(N) until it returns all N events (or we time out). This
    // tolerates both the sealed path (visible after the seal window) and the
    // live_tail_read path (visible after ~the ingestion tick).
    std::vector<chronolog::Event> events;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(max_wait_sec);
    bool got_all = false;
    // Remember the last status so a failure can say WHY. A tail read that returns an
    // error (no keeper answered) and one that legitimately has nothing yet both leave
    // `events` empty, and reporting only the count sends the reader to the wrong place.
    int last_rc = chronolog::CL_SUCCESS;
    while(std::chrono::steady_clock::now() < deadline)
    {
        events.clear();
        last_rc = handle->playback(static_cast<size_t>(event_count), events);
        if(last_rc == chronolog::CL_SUCCESS && events.size() >= static_cast<size_t>(event_count))
        {
            got_all = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    int result = 0;
    if(!got_all)
    {
        std::cerr << "[tail-read-e2e] FAIL: playback() returned " << events.size() << "/" << event_count
                  << " events within " << max_wait_sec << "s"
                  << " (last status: " << chronolog::to_string_client(last_rc) << ")\n";
        if(last_rc != chronolog::CL_SUCCESS)
        {
            std::cerr << "[tail-read-e2e]       the last read FAILED rather than returning an empty tail -- check "
                         "keeper reachability, not the seal window\n";
        }
        result = 1;
    }
    else
    {
        // We wrote exactly N events to a fresh story, so the last-N is all of
        // them. A single writer assigns strictly increasing EventSequence, so the
        // ascending playback order must equal the write order.
        if(events.size() != static_cast<size_t>(event_count))
        {
            std::cerr << "[tail-read-e2e] FAIL: expected exactly " << event_count << " events, got " << events.size()
                      << "\n";
            result = 1;
        }
        for(size_t i = 0; i + 1 < events.size() && result == 0; ++i)
        {
            if(!(events[i] < events[i + 1]))
            {
                std::cerr << "[tail-read-e2e] FAIL: events not in ascending order at index " << i << "\n";
                result = 1;
            }
        }
        for(size_t i = 0; i < events.size() && result == 0; ++i)
        {
            if(events[i].log_record() != expected[i])
            {
                std::cerr << "[tail-read-e2e] FAIL: payload mismatch at " << i << ": got '" << events[i].log_record()
                          << "' expected '" << expected[i] << "'\n";
                result = 1;
            }
        }
        if(result == 0)
            std::cout << "[tail-read-e2e] PASS: playback() returned " << events.size()
                      << " events in correct ascending order with matching payloads\n";
    }

    client.ReleaseStory(chronicle, story);
    client.DestroyStory(chronicle, story);
    client.DestroyChronicle(chronicle);
    client.Disconnect();
    return result;
}
