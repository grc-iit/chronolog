// Client side of tests/integration/keeper_wal_crash_test.sh and
// keeper_restart_held_test.sh: writes events durable or accepted and prints
// the identity of each one the keeper acknowledged, or replays a story and
// prints the identity of every event it returns, so the script can compare
// the two.
//
//   --write <n> --mode durable|accepted [--bytes <n>]
//       acquires the story, logs <n> events through
//       log_event(record, options, event_time) and prints
//       "<time> <client id> <index>" for each one answered CL_SUCCESS. After
//       the last call it prints LAST_ACK_NS <wall clock ns>, then releases the
//       story and prints WRITE_ACKED <acked> <n> and one
//       WRITE_CODE <CL_...> <count> per return code. A call answered with
//       anything else prints "NOT_ACKED <CL_...> <client id> <index>": the
//       client does not return the timestamp of an event it was refused, so a
//       script looks for that client id and index in a replay. Without --bytes the record
//       is "<mode> event <index>". With --bytes it is that many 'x' bytes.
//       --timeout-ms and --attempts override LogEventOptions for a durable
//       write; omitted, the defaults (10000 ms, 3 attempts) stay.
//   --replay
//       acquires the story, replays its whole range, releases it, and prints
//       "<time> <client id> <index>" per returned event, then
//       REPLAY_STATUS <CL_...> and REPLAY_COUNT <n>.
//   --loop --stop-file <path> --release-file <path>
//       acquires the story and prints ACQUIRED <CL_...>, then logs an event
//       every --interval-ms (default 200) until <stop file> exists, odd calls
//       accepted and even calls durable, and prints one line per call:
//       "EVENT accepted|durable <CL_...> <time> <client id> <index> <wall ns>"
//       (time is 0 when the call failed; wall ns is the clock before the
//       call). Then it prints LOOP_STOPPED <calls>, holds the story until
//       <release file> exists, releases it and prints RELEASED. --timeout-ms
//       (default 1000) and --attempts (default 1) go into the durable calls'
//       LogEventOptions, so a durable call to a keeper that is down returns
//       after about timeout_ms.
//   --retry-file <path> --keeper-protocol <p> --keeper-ip <ip>
//       --keeper-port <port> --keeper-provider <id>
//       sends each "<time> <client id> <index>" line in the file again as
//       record_event_durable, with the same record the --write path used
//       ("durable event <index>") and the story id of the chronicle and story
//       names. Prints one "RETRY <CL_...> <time> <client id> <index>" line and
//       then RETRY_OK <ok> <total>. This is the client's retry of an event the
//       keeper already has: the public log_event API would assign a new index.
//   --direct-write --keeper-protocol <p> --keeper-ip <ip>
//       --keeper-port <port> --keeper-provider <id>
//       sends one record_event_durable with client id 1 and index 1 straight
//       to the keeper, without AcquireStory, and prints
//       "DIRECT <CL_...> <time> 1 1" and then DIRECT_MS <ms the RPC took>.
//
// Usage: durable_probe --config <client conf> --chronicle <name> --story <name>
//                      (--write <n> --mode durable|accepted [--bytes <n>] |
//                       --replay |
//                       --loop --stop-file <path> --release-file <path>
//                       [--interval-ms <ms>] [--timeout-ms <ms>] [--attempts <n>] |
//                       (--retry-file <path> | --direct-write) --keeper-protocol <p>
//                       --keeper-ip <ip> --keeper-port <port> --keeper-provider <id>)
//
// The client API does not return an event's index. This client is new and logs
// nothing else, and every call that returns CL_SUCCESS or a keeper's error took
// the next index, so the i-th call (from 1) carries index i. CL_ERR_INVALID_ARG
// returns before the index is taken, and CL_ERR_NO_KEEPERS before or after it,
// so the probe stops at the first call that returns either.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <thallium.hpp>

#include <ClientConfiguration.h>
#include <KeeperRecordingClient.h>
#include <ServiceId.h>
#include <StoryIdOf.h>
#include <chronolog_client.h>
#include <chrono_monitor.h>

namespace tl = thallium;

namespace
{

void usage(char const* name)
{
    std::cerr << "Usage: " << name
              << " --chronicle <name> --story <name>"
                 " (--config <client conf> --write <n> --mode durable|accepted [--bytes <n>]"
                 " [--timeout-ms <ms>] [--attempts <n>] |"
                 " --config <client conf> --replay |"
                 " --config <client conf> --loop --stop-file <path> --release-file <path>"
                 " [--interval-ms <ms>] [--timeout-ms <ms>] [--attempts <n>] |"
                 " --retry-file <path> --keeper-protocol <p> --keeper-ip <ip>"
                 " --keeper-port <port> --keeper-provider <id>)"
              << std::endl;
}

uint64_t wall_clock_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

// --loop: logs events until stop_file exists, odd calls accepted and even
// calls durable, then holds the story until release_file exists. Returns the
// probe's exit code.
int write_loop(chronolog::StoryHandle& story_handle,
               chronolog::ClientId const& client_id,
               std::string const& stop_file,
               std::string const& release_file,
               long interval_ms,
               chronolog::LogEventOptions const& durable_options)
{
    chronolog::LogEventOptions accepted_options = durable_options;
    accepted_options.durability = chronolog::Durability::Accepted;
    long calls = 0;
    int exit_code = EXIT_SUCCESS;
    while(!std::filesystem::exists(stop_file))
    {
        ++calls;
        bool const durable = (calls % 2 == 0);
        std::string const mode = durable ? "durable" : "accepted";
        uint64_t const wall_ns = wall_clock_ns();
        uint64_t event_time = 0;
        int const code = story_handle.log_event(mode + " loop event " + std::to_string(calls),
                                                durable ? durable_options : accepted_options,
                                                event_time);
        std::cout << "EVENT " << mode << " " << chronolog::to_string_client(code) << " " << event_time << " "
                  << client_id << " " << calls << " " << wall_ns << std::endl;
        if(code == chronolog::CL_ERR_NO_KEEPERS || code == chronolog::CL_ERR_INVALID_ARG)
        {
            // later indexes would be unknown, as in the --write loop
            exit_code = EXIT_FAILURE;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    std::cout << "LOOP_STOPPED " << calls << std::endl;
    while(!std::filesystem::exists(release_file)) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    return exit_code;
}

// Resend the identities in `path` to one keeper. Each line is
// "<time> <client id> <index>". The record matches a --write durable event
// with that index, and the story id is the one the keeper checks the names
// against, so a keeper that still has the event answers CL_SUCCESS and does
// not store another copy.
int retry_identities(std::string const& path,
                     std::string const& chronicle,
                     std::string const& story,
                     std::string const& protocol,
                     std::string const& ip,
                     uint16_t port,
                     uint16_t provider)
{
    if(chronolog::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "durable_probe_retry") == 1)
    {
        return EXIT_FAILURE;
    }

    std::ifstream in(path);
    if(!in)
    {
        std::cerr << "[DurableProbe] Cannot read identities in '" << path << "'" << std::endl;
        return EXIT_FAILURE;
    }

    tl::engine engine(protocol + "://" + ip, THALLIUM_CLIENT_MODE, true, 1);
    chronolog::ServiceId const service(protocol, ip, port, provider);
    std::unique_ptr<chronolog::KeeperRecordingClient> keeper(
            chronolog::KeeperRecordingClient::CreateKeeperRecordingClient(engine, service));
    if(keeper == nullptr)
    {
        std::cout << "RETRY_OK 0 0" << std::endl;
        return EXIT_FAILURE;
    }

    chronolog::StoryId const story_id = chronolog::storyIdOf(chronicle, story);
    long ok = 0;
    long total = 0;
    std::string line;
    while(std::getline(in, line))
    {
        if(line.empty())
        {
            continue;
        }
        unsigned long long time = 0;
        unsigned long long client = 0;
        unsigned long index = 0;
        if(std::sscanf(line.c_str(), "%llu %llu %lu", &time, &client, &index) != 3)
        {
            std::cerr << "[DurableProbe] Bad identity line: " << line << std::endl;
            std::cout << "RETRY_OK " << ok << " " << total << std::endl;
            return EXIT_FAILURE;
        }
        ++total;
        chronolog::LogEvent event(story_id,
                                  static_cast<uint64_t>(time),
                                  static_cast<chronolog::ClientId>(client),
                                  static_cast<uint32_t>(index),
                                  "durable event " + std::to_string(index));
        int keeper_code = chronolog::CL_ERR_UNKNOWN;
        chronolog::DurableSendOutcome const outcome =
                keeper->send_durable_event_msg(chronicle, story, event, 10000, keeper_code);
        if(outcome != chronolog::DurableSendOutcome::Answered)
        {
            keeper_code = chronolog::CL_ERR_DURABILITY_UNKNOWN;
        }
        if(keeper_code == chronolog::CL_SUCCESS)
        {
            ++ok;
        }
        std::cout << "RETRY " << chronolog::to_string_client(keeper_code) << " " << time << " " << client << " "
                  << index << std::endl;
    }
    std::cout << "RETRY_OK " << ok << " " << total << std::endl;
    return (total > 0 && ok == total) ? EXIT_SUCCESS : EXIT_FAILURE;
}

// One record_event_durable sent straight to a keeper, not through AcquireStory.
// Shutdown unregisters the keeper before it refuses new writes, so a client
// that asks the visor for the story no longer reaches the RPC that answers
// CL_ERR_DURABILITY_UNAVAILABLE.
int direct_write(std::string const& chronicle,
                 std::string const& story,
                 std::string const& protocol,
                 std::string const& ip,
                 uint16_t port,
                 uint16_t provider)
{
    if(chronolog::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "durable_probe_direct") == 1)
    {
        return EXIT_FAILURE;
    }
    tl::engine engine(protocol + "://" + ip, THALLIUM_CLIENT_MODE, true, 1);
    chronolog::ServiceId const service(protocol, ip, port, provider);
    std::unique_ptr<chronolog::KeeperRecordingClient> keeper(
            chronolog::KeeperRecordingClient::CreateKeeperRecordingClient(engine, service));
    if(keeper == nullptr)
    {
        std::cout << "DIRECT CL_ERR_UNKNOWN 0 0 0" << std::endl;
        return EXIT_FAILURE;
    }
    uint64_t const time = wall_clock_ns();
    chronolog::LogEvent event(chronolog::storyIdOf(chronicle, story), time, 1, 1, "direct event");
    int keeper_code = chronolog::CL_ERR_UNKNOWN;
    auto const start = std::chrono::steady_clock::now();
    chronolog::DurableSendOutcome const outcome =
            keeper->send_durable_event_msg(chronicle, story, event, 2000, keeper_code);
    auto const took =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if(outcome != chronolog::DurableSendOutcome::Answered)
    {
        keeper_code = chronolog::CL_ERR_DURABILITY_UNKNOWN;
    }
    std::cout << "DIRECT " << chronolog::to_string_client(keeper_code) << " " << time << " 1 1" << std::endl;
    std::cout << "DIRECT_MS " << took << std::endl;
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv)
{
    std::string conf_file_path;
    std::string chronicle_name;
    std::string story_name;
    std::string mode;
    std::string stop_file;
    std::string release_file;
    std::string retry_file;
    std::string keeper_protocol;
    std::string keeper_ip;
    long write_count = -1;
    long payload_bytes = 0;
    long interval_ms = 200;
    long timeout_ms = 1000;
    long attempts = 1;
    long keeper_port = -1;
    long keeper_provider = -1;
    bool replay = false;
    bool loop = false;
    bool direct = false;
    bool timeout_given = false;
    bool attempts_given = false;

    struct option long_options[] = {{"config", required_argument, nullptr, 'c'},
                                    {"chronicle", required_argument, nullptr, 'r'},
                                    {"story", required_argument, nullptr, 's'},
                                    {"write", required_argument, nullptr, 'w'},
                                    {"mode", required_argument, nullptr, 'm'},
                                    {"replay", no_argument, nullptr, 'p'},
                                    {"loop", no_argument, nullptr, 'l'},
                                    {"stop-file", required_argument, nullptr, 'S'},
                                    {"release-file", required_argument, nullptr, 'R'},
                                    {"interval-ms", required_argument, nullptr, 'i'},
                                    {"timeout-ms", required_argument, nullptr, 't'},
                                    {"attempts", required_argument, nullptr, 'a'},
                                    {"bytes", required_argument, nullptr, 'b'},
                                    {"retry-file", required_argument, nullptr, 'f'},
                                    {"keeper-protocol", required_argument, nullptr, 'P'},
                                    {"keeper-ip", required_argument, nullptr, 'I'},
                                    {"keeper-port", required_argument, nullptr, 'K'},
                                    {"keeper-provider", required_argument, nullptr, 'V'},
                                    {"direct-write", no_argument, nullptr, 'd'},
                                    {nullptr, 0, nullptr, 0}};
    int opt;
    while((opt = getopt_long(argc, argv, "c:", long_options, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                conf_file_path = optarg;
                break;
            case 'r':
                chronicle_name = optarg;
                break;
            case 's':
                story_name = optarg;
                break;
            case 'w':
                write_count = std::strtol(optarg, nullptr, 10);
                break;
            case 'm':
                mode = optarg;
                break;
            case 'p':
                replay = true;
                break;
            case 'l':
                loop = true;
                break;
            case 'S':
                stop_file = optarg;
                break;
            case 'R':
                release_file = optarg;
                break;
            case 'i':
                interval_ms = std::strtol(optarg, nullptr, 10);
                break;
            case 't':
                timeout_ms = std::strtol(optarg, nullptr, 10);
                timeout_given = true;
                break;
            case 'a':
                attempts = std::strtol(optarg, nullptr, 10);
                attempts_given = true;
                break;
            case 'b':
                payload_bytes = std::strtol(optarg, nullptr, 10);
                break;
            case 'f':
                retry_file = optarg;
                break;
            case 'P':
                keeper_protocol = optarg;
                break;
            case 'I':
                keeper_ip = optarg;
                break;
            case 'K':
                keeper_port = std::strtol(optarg, nullptr, 10);
                break;
            case 'V':
                keeper_provider = std::strtol(optarg, nullptr, 10);
                break;
            case 'd':
                direct = true;
                break;
            default:
                usage(argv[0]);
                return EXIT_FAILURE;
        }
    }
    bool const write = (write_count >= 0);
    bool const retry = !retry_file.empty();
    bool const keeper_given = !keeper_protocol.empty() && !keeper_ip.empty() && keeper_port > 0 &&
                              keeper_port <= 65535 && keeper_provider >= 0 && keeper_provider <= 65535;
    if(chronicle_name.empty() || story_name.empty() ||
       (int(write) + int(replay) + int(loop) + int(retry) + int(direct)) != 1 ||
       (write && ((mode != "durable" && mode != "accepted") || payload_bytes < 0)) ||
       (loop && (stop_file.empty() || release_file.empty() || interval_ms < 0 || timeout_ms <= 0 || attempts <= 0)) ||
       ((retry || direct) && !keeper_given))
    {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if(retry || direct)
    {
        if(retry)
        {
            return retry_identities(retry_file,
                                    chronicle_name,
                                    story_name,
                                    keeper_protocol,
                                    keeper_ip,
                                    static_cast<uint16_t>(keeper_port),
                                    static_cast<uint16_t>(keeper_provider));
        }
        return direct_write(chronicle_name,
                            story_name,
                            keeper_protocol,
                            keeper_ip,
                            static_cast<uint16_t>(keeper_port),
                            static_cast<uint16_t>(keeper_provider));
    }

    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty() && !confManager.load_from_file(conf_file_path))
    {
        std::cerr << "[DurableProbe] Failed to load configuration file '" << conf_file_path << "'" << std::endl;
        return EXIT_FAILURE;
    }
    if(chronolog::chrono_monitor::initialize(confManager.LOG_CONF.LOGTYPE,
                                             confManager.LOG_CONF.LOGFILE,
                                             confManager.LOG_CONF.LOGLEVEL,
                                             confManager.LOG_CONF.LOGNAME,
                                             confManager.LOG_CONF.LOGFILESIZE,
                                             confManager.LOG_CONF.LOGFILENUM,
                                             confManager.LOG_CONF.FLUSHLEVEL) == 1)
    {
        return EXIT_FAILURE;
    }

    chronolog::ClientPortalServiceConf portalConf;
    portalConf.PROTO_CONF = confManager.PORTAL_CONF.PROTO_CONF;
    portalConf.IP = confManager.PORTAL_CONF.IP;
    portalConf.PORT = confManager.PORTAL_CONF.PORT;
    portalConf.PROVIDER_ID = confManager.PORTAL_CONF.PROVIDER_ID;

    chronolog::ClientQueryServiceConf queryConf;
    queryConf.PROTO_CONF = confManager.QUERY_CONF.PROTO_CONF;
    queryConf.IP = confManager.QUERY_CONF.IP;
    queryConf.PORT = confManager.QUERY_CONF.PORT;
    queryConf.PROVIDER_ID = confManager.QUERY_CONF.PROVIDER_ID;

    std::string const status_key = replay ? "REPLAY_STATUS " : "WRITE_STATUS ";
    chronolog::Client client(portalConf, queryConf);
    int ret = client.Connect();
    if(ret != chronolog::CL_SUCCESS)
    {
        std::cout << status_key << chronolog::to_string_client(ret) << std::endl;
        return EXIT_FAILURE;
    }

    // the chronicle may exist from an earlier run; a replay of a story that
    // arrived from a WAL still needs the chronicle to exist
    client.CreateChronicle(chronicle_name);
    auto acquire_result = client.AcquireStory(chronicle_name, story_name);
    if(loop)
    {
        std::cout << "ACQUIRED " << chronolog::to_string_client(acquire_result.first) << std::endl;
    }
    if(acquire_result.first != chronolog::CL_SUCCESS)
    {
        std::cout << status_key << chronolog::to_string_client(acquire_result.first) << std::endl;
        client.Disconnect();
        return EXIT_FAILURE;
    }

    if(loop)
    {
        chronolog::LogEventOptions durable_options;
        durable_options.durability = chronolog::Durability::Durable;
        durable_options.timeout_ms = static_cast<uint32_t>(timeout_ms);
        durable_options.attempts = static_cast<uint32_t>(attempts);
        int const exit_code = write_loop(*acquire_result.second,
                                         client.client_id(),
                                         stop_file,
                                         release_file,
                                         interval_ms,
                                         durable_options);
        client.ReleaseStory(chronicle_name, story_name);
        client.Disconnect();
        std::cout << "RELEASED" << std::endl;
        return exit_code;
    }

    if(replay)
    {
        std::vector<chronolog::Event> events;
        ret = client.ReplayStory(chronicle_name, story_name, 1, 2000000000000000000ULL, events);
        for(auto const& event: events)
        {
            std::cout << event.time() << " " << event.client_id() << " " << event.index() << std::endl;
        }
        std::cout << "REPLAY_STATUS " << chronolog::to_string_client(ret) << std::endl;
        std::cout << "REPLAY_COUNT " << events.size() << std::endl;
        client.ReleaseStory(chronicle_name, story_name);
        client.Disconnect();
        return (ret == chronolog::CL_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    chronolog::LogEventOptions options;
    options.durability = (mode == "durable") ? chronolog::Durability::Durable : chronolog::Durability::Accepted;
    if(timeout_given)
    {
        options.timeout_ms = static_cast<uint32_t>(timeout_ms);
    }
    if(attempts_given)
    {
        options.attempts = static_cast<uint32_t>(attempts);
    }
    chronolog::ClientId const client_id = client.client_id();
    std::map<int, long> codes;
    long acked = 0;
    bool stopped_early = false;
    std::string const wide(static_cast<std::size_t>(payload_bytes), 'x');
    for(long i = 1; i <= write_count; ++i)
    {
        uint64_t event_time = 0;
        std::string const record = (payload_bytes > 0) ? wide : mode + " event " + std::to_string(i);
        int const code = acquire_result.second->log_event(record, options, event_time);
        ++codes[code];
        if(code == chronolog::CL_SUCCESS)
        {
            ++acked;
            std::cout << event_time << " " << client_id << " " << i << std::endl;
        }
        else if(code == chronolog::CL_ERR_NO_KEEPERS || code == chronolog::CL_ERR_INVALID_ARG)
        {
            // CL_ERR_INVALID_ARG returns before the client takes an index, and
            // CL_ERR_NO_KEEPERS before it (no keepers for the story) or after it
            // (no keeper chosen): later indexes would be unknown
            stopped_early = true;
            break;
        }
        else
        {
            std::cout << "NOT_ACKED " << chronolog::to_string_client(code) << " " << client_id << " " << i << std::endl;
        }
    }
    std::cout << "LAST_ACK_NS " << wall_clock_ns() << std::endl;

    client.ReleaseStory(chronicle_name, story_name);
    client.Disconnect();
    std::cout << "WRITE_ACKED " << acked << " " << write_count << std::endl;
    for(auto const& [code, count]: codes)
    {
        std::cout << "WRITE_CODE " << chronolog::to_string_client(code) << " " << count << std::endl;
    }
    std::cout << status_key << (stopped_early ? "STOPPED_EARLY" : "DONE") << std::endl;
    return (!stopped_early && acked == write_count) ? EXIT_SUCCESS : EXIT_FAILURE;
}
