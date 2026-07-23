#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <pwd.h>
#include <getopt.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <mpi.h>
#include <margo.h>

#include <chronolog_client.h>
#include <ClientConfiguration.h>
#include <chrono_monitor.h>
#include <cmd_arg_parse.h>
#include <TimerWrapper.h>
#include <common.h>

// Scripted ChronoLog performance/regression test.
// Extracted from tools/cli/client_admin.cpp; the interactive shell counterpart
// lives in tools/cli/client_cli.cpp.

#define MAX_EVENT_SIZE (32 * 1024 * 1024)

typedef struct workload_conf_args_
{
    uint64_t chronicle_count = 1;
    uint64_t story_count = 1;
    uint64_t min_event_size = 0;
    uint64_t ave_event_size = 256;
    uint64_t max_event_size = 512;
    uint64_t event_count = 1;
    uint64_t event_interval = 0;
    bool barrier = false;
    std::string event_payload_file;
    bool write = true;
    bool read = false;
    bool shared_story = false;
    bool perf_test = false;
    // Latency (send->visible-via-playback) test knobs
    bool latency_test = false;
    uint64_t playback_n = 0;         // last-N events fetched per playback() poll (0 = auto)
    uint64_t poll_interval_ms = 500; // sleep between playback() polls
    uint64_t max_wait_sec = 180;     // give up waiting for an event to seal after this
} workload_conf_args;

static void usage(char** argv)
{
    std::cerr << "\nUsage: " << argv[0]
              << " [options]\n"
                 "-c|--config <config_file>\n"
                 "-w|--write\t\tRecord events\n"
                 "-r|--read\t\tRetrieve events\n"
                 "-l|--latency\t\tMeasure send->visible-via-playback() latency\n"
                 "-h|--chronicle_count <chronicle_count_per_proc>\n"
                 "-t|--story_count <story_count_per_proc>\n"
                 "-a|--min_event_size <min_event_size_in_byte>\n"
                 "-s|--ave_event_size <ave_event_size_in_byte>\n"
                 "-b|--max_event_size <max_event_size_in_byte>\n"
                 "-n|--event_count <event_count_per_proc>\n"
                 "-g|--event_interval <event_interval_in_us>\n"
                 "-y|--barrier\t\tHold next API call until completion of previous one\n"
                 "-f|--event_payload_file <event_payload_file>\n"
                 "-o|--shared_story\tAll procs record events to the same chronicle\n"
                 "-p|--perf\t\tReport performance metrics after completion\n"
                 "-k|--playback_n <n>\t\t(latency) last-N events fetched per playback() poll (0=auto)\n"
                 "-e|--poll_interval <ms>\t(latency) sleep between playback() polls in ms\n"
                 "-m|--max_wait <sec>\t\t(latency) give up waiting for an event to seal after this\n"
                 "-u|--usage\t\tPrint this page\n"
              << std::endl;
}

[[maybe_unused]] static void random_sleep()
{
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    srand(getpid());
    long usec = random() % 100000;
    LOG_DEBUG("Sleeping for {} us ...", usec);
    usleep(usec * rank);
}

static uint64_t get_uint64_t_from_string(const std::string& str)
{
    char* endptr;
    errno = 0; // Reset errno before calling strtoull
    uint64_t value = strtoull(str.c_str(), &endptr, 10);
    if(*endptr != '\0')
    {
        std::cerr << "Invalid number: " << str << std::endl;
        exit(EXIT_FAILURE);
    }
    if(value == 0 || errno == ERANGE)
    {
        std::cerr << "Only positive number is allowed" << std::endl;
        exit(EXIT_FAILURE);
    }
    return value;
}

static std::pair<std::string, workload_conf_args> cmd_arg_parse(int argc, char** argv)
{
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    int opt;
    char* config_file = nullptr;
    workload_conf_args workload_args;

    struct option long_options[] = {{"config", required_argument, nullptr, 'c'},
                                    {"write", no_argument, nullptr, 'w'},
                                    {"read", optional_argument, nullptr, 'r'},
                                    {"latency", no_argument, nullptr, 'l'},
                                    {"chronicle_count", required_argument, nullptr, 'h'},
                                    {"story_count", required_argument, nullptr, 't'},
                                    {"min_event_size", required_argument, nullptr, 'a'},
                                    {"ave_event_size", required_argument, nullptr, 's'},
                                    {"max_event_size", required_argument, nullptr, 'b'},
                                    {"event_count", required_argument, nullptr, 'n'},
                                    {"event_interval", required_argument, nullptr, 'g'},
                                    {"barrier", optional_argument, nullptr, 'y'},
                                    {"event_payload_file", optional_argument, nullptr, 'f'},
                                    {"shared_story", optional_argument, nullptr, 'o'},
                                    {"perf", optional_argument, nullptr, 'p'},
                                    {"playback_n", required_argument, nullptr, 'k'},
                                    {"poll_interval", required_argument, nullptr, 'e'},
                                    {"max_wait", required_argument, nullptr, 'm'},
                                    {"usage", no_argument, nullptr, 'u'},
                                    {nullptr, 0, nullptr, 0}};

    while((opt = getopt_long(argc, argv, "c:wrlh:t:a:s:b:n:g:yf:opk:e:m:u", long_options, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                config_file = optarg;
                break;
            case 'w':
                workload_args.write = true;
                break;
            case 'r':
                workload_args.read = true;
                workload_args.write = false; // read mode has higher priority than write mode
                break;
            case 'l':
                workload_args.latency_test = true;
                workload_args.write = false; // latency mode drives its own write+poll loop
                workload_args.read = false;
                break;
            case 'h':
                workload_args.chronicle_count = get_uint64_t_from_string(optarg);
                break;
            case 't':
                workload_args.story_count = get_uint64_t_from_string(optarg);
                break;
            case 'a':
                workload_args.min_event_size = get_uint64_t_from_string(optarg);
                break;
            case 's':
                workload_args.ave_event_size = get_uint64_t_from_string(optarg);
                break;
            case 'b':
                workload_args.max_event_size = get_uint64_t_from_string(optarg);
                break;
            case 'n':
                workload_args.event_count = get_uint64_t_from_string(optarg);
                break;
            case 'g':
                workload_args.event_interval = get_uint64_t_from_string(optarg);
                break;
            case 'y':
                workload_args.barrier = true;
                break;
            case 'f':
                workload_args.event_payload_file = optarg;
                break;
            case 'o':
                workload_args.shared_story = true;
                break;
            case 'p':
                workload_args.perf_test = true;
                break;
            case 'k':
                workload_args.playback_n = get_uint64_t_from_string(optarg);
                break;
            case 'e':
                workload_args.poll_interval_ms = get_uint64_t_from_string(optarg);
                break;
            case 'm':
                workload_args.max_wait_sec = get_uint64_t_from_string(optarg);
                break;
            case 'u':
                if(rank == 0)
                {
                    usage(argv);
                }
                exit(EXIT_SUCCESS);
            case '?':
                usage(argv);
                exit(EXIT_FAILURE);
            default:
                std::cerr << "Unknown option: " << opt << std::endl;
                exit(EXIT_FAILURE);
        }
    }

    if(config_file)
    {
        if(rank == 0)
        {
            std::cout << "Config file specified: " << config_file << std::endl;
            std::cout << "Number of processes: " << size << std::endl;
            std::cout << "Chronicle count (#chronicles per proc): " << workload_args.chronicle_count << std::endl;
            std::cout << "Story count (#stories per proc): " << workload_args.story_count << std::endl;
            if(!workload_args.event_payload_file.empty())
            {
                std::cout << "Event payload file specified (event payloads in lines): "
                          << workload_args.event_payload_file.c_str() << std::endl;
            }
            else
            {
                std::cout << "No event payload file specified, use default/specified separate conf args ..."
                          << std::endl;
                std::cout << "Min event size (minimum of event payload length following a normal distribution): "
                          << workload_args.min_event_size << " bytes" << std::endl;
                std::cout << "Ave event size (median of event payload length following a normal distribution): "
                          << workload_args.ave_event_size << " bytes" << std::endl;
                std::cout << "Max event size (maximum of event payload length following a normal distribution): "
                          << workload_args.max_event_size << " bytes" << std::endl;
                std::cout << "Event count (#events per proc): " << workload_args.event_count << std::endl;
                std::cout << "Event interval (time in-between event accesses): " << workload_args.event_interval
                          << " us" << std::endl;
            }
            std::cout << "Barrier (hold on next ChronoLog API call until completion of the previous one): "
                      << (workload_args.barrier ? "true" : "false") << std::endl;
            std::cout << "Shared story (all procs access the same story): "
                      << (workload_args.shared_story ? "true" : "false") << std::endl;
        }
        return {std::pair<std::string, workload_conf_args>((config_file), workload_args)};
    }
    else
    {
        if(rank == 0)
        {
            std::cout << "No config file specified, using default settings instead:" << std::endl;
            std::cout << "Number of processes: " << size << std::endl;
            std::cout << "Chronicle count (#chronicles per proc): " << workload_args.chronicle_count << std::endl;
            std::cout << "Story count (#stories per proc) : " << workload_args.story_count << std::endl;
            std::cout << "Min event size (minimum of event payload length following a normal distribution): "
                      << workload_args.min_event_size << " bytes" << std::endl;
            std::cout << "Ave event size (median of event payload length following a normal distribution): "
                      << workload_args.ave_event_size << " bytes" << std::endl;
            std::cout << "Max event size (maximum of event payload length following a normal distribution): "
                      << workload_args.max_event_size << " bytes" << std::endl;
            std::cout << "Event count (#events per proc): " << workload_args.event_count << std::endl;
            std::cout << "Event interval (time in-between event accesses): " << workload_args.event_interval << " us"
                      << std::endl;
            std::cout << "Barrier (hold on next data access until completion of the previous one): "
                      << (workload_args.barrier ? "true" : "false") << std::endl;
            std::cout << "Shared story (all procs access the same story): "
                      << (workload_args.shared_story ? "true" : "false") << std::endl;
        }
        return {"", workload_args};
    }
}

static int test_create_chronicle(chronolog::Client& client, const std::string& chronicle_name)
{
    int ret = client.CreateChronicle(chronicle_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_CHRONICLE_EXISTS);
    return ret;
}

static std::pair<int, chronolog::StoryHandle*>
test_acquire_story(chronolog::Client& client, const std::string& chronicle_name, const std::string& story_name)
{
    return client.AcquireStory(chronicle_name, story_name);
}

static uint64_t test_write_event(chronolog::StoryHandle* story_handle, const std::string& event_payload)
{
    uint64_t ret = story_handle->log_event(event_payload);
    assert(ret > 0);
    return ret;
}

static int test_replay_story(chronolog::Client& client,
                             const std::string& chronicle_name,
                             const std::string& story_name,
                             uint64_t start_time,
                             uint64_t end_time,
                             std::vector<chronolog::Event>& replay_events)
{
    int ret = client.ReplayStory(chronicle_name, story_name, start_time, end_time, replay_events);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST);
    return ret;
}

static int
test_release_story(chronolog::Client& client, const std::string& chronicle_name, const std::string& story_name)
{
    int ret = client.ReleaseStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST);
    return ret;
}

static int test_destroy_chronicle(chronolog::Client& client, const std::string& chronicle_name)
{
    int ret = client.DestroyChronicle(chronicle_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
    return ret;
}

static int
test_destroy_story(chronolog::Client& client, const std::string& chronicle_name, const std::string& story_name)
{
    int ret = client.DestroyStory(chronicle_name, story_name);
    assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_NOT_EXIST || ret == chronolog::CL_ERR_ACQUIRED);
    return ret;
}

static uint64_t get_event_timestamp(std::string& event_line)
{
    /*
     * Supported log files: syslog, auth.log, kern.log, ufw.log on Ubuntu
     * Expected format of log record:
     * Nov  5 14:36:49 ares-comp-01 systemd[1]: Started Time & Date Service.
     */
    size_t pos_first_space = event_line.find_first_of("0123456789") - 1;
    size_t pos_second_space = event_line.find_first_of(' ', pos_first_space + 1);
    size_t pos_third_space = event_line.find_first_of(' ', pos_second_space + 1);

    std::string timestamp_str = event_line.substr(0, pos_third_space);
    std::tm timeinfo{};
    auto now = std::chrono::system_clock::now();
    std::time_t current_time = std::chrono::system_clock::to_time_t(now);
    std::tm* time_info_now = std::localtime(&current_time);
    // assume the log file is generated in the same year as the current time
    timeinfo.tm_year = time_info_now->tm_year;
    strptime(timestamp_str.c_str(), "%b %d %H:%M:%S", &timeinfo);
    uint64_t timestamp = timelocal(&timeinfo);
    return timestamp;
}

static uint64_t get_bigbang_timestamp(std::ifstream& file)
{
    std::string line;
    std::getline(file, line);
    uint64_t bigbang_timestamp = get_event_timestamp(line);
    file.seekg(0, std::ios::beg);
    return bigbang_timestamp;
}

// ---------------------------------------------------------------------------
// Latency (send -> visible-via-playback) measurement
//
// StoryHandle::playback(n, events) returns the most recent `n` events of a
// story from the keepers' in-memory tail, but only after a chunk SEALS
// (chunk_duration + acceptance_window). The wall-clock gap between logging an
// event and it first showing up in playback() is therefore a "freshness"
// latency floored by that sealing window (~25-30s on the default local deploy),
// not an RPC round-trip.
//
// log_event() returns the event's assigned timestamp, which is
// high_resolution_clock::now().time_since_epoch().count() (ns) and is exactly
// the value reported by Event::time() on playback. Because writer and poller run
// in the same process (one MPI rank) on the same clock, the first-appearance
// latency is simply now_ns() - event.time() with no clock-skew correction.
// ---------------------------------------------------------------------------

// Same clock domain as chronolog::ChronologTimer::getTimestamp().
static uint64_t now_ns()
{
    return static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

// Linear-interpolated percentile over an already-sorted vector.
static double percentile(const std::vector<double>& sorted, double p)
{
    if(sorted.empty())
        return 0.0;
    if(sorted.size() == 1)
        return sorted.front();
    double rank = p * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(rank);
    size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac = rank - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// Log `event_count` events to `story_handle`, then poll playback() until each
// of THIS client's events first becomes visible, recording per-event
// send->visible latency (ms). Appends to `latencies_ms`; bumps logged/seen.
static void latency_measure_story(chronolog::StoryHandle* story_handle,
                                  chronolog::ClientId my_client_id,
                                  const workload_conf_args& workload_args,
                                  uint64_t event_count,
                                  size_t playback_n,
                                  const std::string& payload_str,
                                  std::vector<double>& latencies_ms,
                                  uint64_t& logged_count,
                                  uint64_t& seen_count)
{
    // Single interleaved loop: write events on the event-interval schedule while
    // polling playback() on the poll-interval schedule. Interleaving matters when
    // the write phase spans more than one chunk window (event_interval large): an
    // event that seals mid-write must be caught at its true first appearance, not
    // at the first poll after all writes finished. eventTime is monotonically
    // assigned per log_event() call, so it uniquely keys an event of this client.
    std::set<uint64_t> pending; // my logged eventTimes not yet seen via playback
    uint64_t event_size = std::min(workload_args.ave_event_size, static_cast<uint64_t>(payload_str.size()));
    std::string event_payload = payload_str.substr(0, event_size);

    uint64_t write_interval_ns = workload_args.event_interval * 1000ULL;     // us -> ns
    uint64_t poll_interval_ns = workload_args.poll_interval_ms * 1000000ULL; // ms -> ns
    uint64_t written = 0;
    uint64_t next_write_ns = now_ns(); // first event immediately
    uint64_t next_poll_ns = now_ns();
    uint64_t deadline_ns = 0; // countdown starts once the last event is written

    while(true)
    {
        uint64_t now = now_ns();

        // Write due events (a burst when event_interval == 0).
        while(written < event_count && now >= next_write_ns)
        {
            uint64_t event_time = test_write_event(story_handle, event_payload);
            if(event_time != 0)
            {
                pending.insert(event_time);
                ++logged_count;
            }
            ++written;
            next_write_ns = now + write_interval_ns;
            if(written == event_count)
                deadline_ns = now_ns() + workload_args.max_wait_sec * 1000000000ULL;
            now = now_ns();
        }

        // Poll playback() and record first-appearance latency for pending events.
        if(now >= next_poll_ns)
        {
            std::vector<chronolog::Event> events;
            int rc = story_handle->playback(playback_n, events);
            if(rc == chronolog::CL_SUCCESS)
            {
                uint64_t t_seen = now_ns();
                for(const auto& event: events)
                {
                    if(event.client_id() != my_client_id)
                        continue; // another client's event (shared story)
                    auto it = pending.find(event.time());
                    if(it == pending.end())
                        continue; // not mine, or already recorded
                    latencies_ms.push_back(static_cast<double>(t_seen - event.time()) / 1e6);
                    ++seen_count;
                    pending.erase(it);
                }
            }
            next_poll_ns = now_ns() + poll_interval_ns;
        }

        // Done once every event is written and either all seen or we time out.
        if(written == event_count && (pending.empty() || now_ns() >= deadline_ns))
            break;

        // Sleep until the next scheduled write or poll to avoid busy spinning.
        uint64_t wake_ns = next_poll_ns;
        if(written < event_count)
            wake_ns = std::min(wake_ns, next_write_ns);
        now = now_ns();
        if(wake_ns > now)
            usleep((wake_ns - now) / 1000ULL);
    }
}

// Aggregate per-rank latency samples on rank 0 and print the distribution.
static void report_latency(int rank,
                           int size,
                           const std::vector<double>& local_latencies_ms,
                           uint64_t local_logged,
                           uint64_t local_seen)
{
    int local_n = static_cast<int>(local_latencies_ms.size());
    std::vector<int> counts(rank == 0 ? size : 0);
    MPI_Gather(&local_n, 1, MPI_INT, counts.data(), 1, MPI_INT, 0, MPI_COMM_WORLD);

    std::vector<int> displs;
    int total_n = 0;
    if(rank == 0)
    {
        displs.resize(size);
        for(int i = 0; i < size; i++)
        {
            displs[i] = total_n;
            total_n += counts[i];
        }
    }
    std::vector<double> all_latencies(rank == 0 ? total_n : 0);
    MPI_Gatherv(local_latencies_ms.data(),
                local_n,
                MPI_DOUBLE,
                all_latencies.data(),
                counts.data(),
                displs.data(),
                MPI_DOUBLE,
                0,
                MPI_COMM_WORLD);

    uint64_t total_logged = 0, total_seen = 0;
    MPI_Reduce(&local_logged, &total_logged, 1, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_seen, &total_seen, 1, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD);

    if(rank != 0)
        return;

    uint64_t never_seen = (total_logged > total_seen) ? (total_logged - total_seen) : 0;
    std::cout << "==================================================================" << std::endl;
    std::cout << "===========  Latency (send -> visible via playback):  ============" << std::endl;
    std::cout << "==================================================================" << std::endl;
    std::cout << "Events logged: " << total_logged << ", seen: " << total_seen
              << ", never seen (unsealed/evicted): " << never_seen << std::endl;
    if(all_latencies.empty())
    {
        std::cout << "No events became visible via playback() within the max wait window." << std::endl;
        std::cout << "Check that keepers are up and increase -m/--max_wait beyond the chunk sealing window."
                  << std::endl;
        return;
    }
    std::sort(all_latencies.begin(), all_latencies.end());
    double sum = 0.0;
    for(double d: all_latencies) sum += d;
    double mean = sum / static_cast<double>(all_latencies.size());
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Latency (ms): min=" << all_latencies.front() << "  mean=" << mean
              << "  median=" << percentile(all_latencies, 0.50) << std::endl;
    std::cout << "              p90=" << percentile(all_latencies, 0.90) << "  p99=" << percentile(all_latencies, 0.99)
              << "  max=" << all_latencies.back() << std::endl;
}

int main(int argc, char** argv)
{
    // To suppress argobots warning
    std::string argobots_conf_str = R"({"argobots" : {"abt_mem_max_num_stacks" : 8
                                                    , "abt_thread_stacksize" : 2097152}})";
    margo_set_environment(argobots_conf_str.c_str());

    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    std::pair<std::string, workload_conf_args> cmd_args = cmd_arg_parse(argc, argv);
    std::string conf_file_path = cmd_args.first;
    workload_conf_args workload_args = cmd_args.second;

    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty())
    {
        if(!confManager.load_from_file(conf_file_path))
        {
            if(rank == 0)
            {
                std::cerr << "[PerformanceTest] Failed to load configuration file '" << conf_file_path
                          << "'. Using default values instead." << std::endl;
            }
        }
        else
        {
            if(rank == 0)
            {
                std::cout << "[PerformanceTest] Configuration file loaded successfully from '" << conf_file_path << "'."
                          << std::endl;
            }
        }
    }
    else
    {
        if(rank == 0)
        {
            std::cout << "[PerformanceTest] No configuration file provided. Using default values." << std::endl;
        }
    }

    if(rank == 0)
    {
        confManager.log_configuration(std::cout);
    }

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
        exit(EXIT_FAILURE);
    }

    if(workload_args.latency_test)
    {
        std::cout << "[PerformanceTest] Running in latency mode (send -> visible via playback())." << std::endl;
    }
    else if(workload_args.read)
    {
        std::cout << "[PerformanceTest] Running in read mode." << std::endl;
    }
    else
    {
        std::cout << "[PerformanceTest] Running in write mode." << std::endl;
    }

    chronolog::Client client = workload_args.read ? chronolog::Client(confManager.PORTAL_CONF, confManager.QUERY_CONF)
                                                  : chronolog::Client(confManager.PORTAL_CONF);
    chronolog::StoryHandle* story_handle;

    TimerWrapper connectTimer(workload_args.perf_test, "Connect");
    TimerWrapper createChronicleTimer(workload_args.perf_test, "CreateChronicle");
    TimerWrapper acquireStoryTimer(workload_args.perf_test, "AcquireStory");
    TimerWrapper writeEventTimer(workload_args.perf_test, "WriteEvent");
    TimerWrapper replayStoryTimer(workload_args.perf_test, "ReplayStory");
    TimerWrapper releaseStoryTimer(workload_args.perf_test, "ReleaseStory");
    TimerWrapper destroyStoryTimer(workload_args.perf_test, "DestroyStory");
    TimerWrapper destroyChronicleTimer(workload_args.perf_test, "DestroyChronicle");
    TimerWrapper disconnectTimer(workload_args.perf_test, "Disconnect");

    int ret_i;
    uint64_t ret_u;
    uint64_t event_payload_size_per_rank = 0;

    // Latency-mode accumulators (per rank)
    std::vector<double> latency_samples_ms;
    uint64_t latency_logged = 0;
    uint64_t latency_seen = 0;

    std::string client_id = gen_random(8);

    const std::string& server_protoc = confManager.PORTAL_CONF.PROTO_CONF;
    const std::string& server_ip = confManager.PORTAL_CONF.IP;
    std::string server_port = std::to_string(confManager.PORTAL_CONF.PORT);
    std::string server_provider_id = std::to_string(confManager.PORTAL_CONF.PROVIDER_ID);
    std::string server_address = server_protoc + "://" + server_ip + ":" + server_port + "@" + server_provider_id;

    std::string username = getpwuid(getuid())->pw_name;
    ret_i = connectTimer.timeBlock(&chronolog::Client::Connect, client);
    assert(ret_i == chronolog::CL_SUCCESS);

    if(rank == 0)
    {
        std::cout << "Connected to server address: " << server_address << std::endl;
    }

    std::string payload_str(MAX_EVENT_SIZE, 'a');

    double local_e2e_start = MPI_Wtime();
    {
        // Non-interactive mode, execute workload via command line arguments
        std::random_device rand_device;
        std::mt19937 gen(rand_device());
        std::uniform_int_distribution char_dist(0, 255);
        double sigma = (double)(workload_args.max_event_size - workload_args.min_event_size) / 6;
        std::normal_distribution<double> size_dist((double)workload_args.ave_event_size, sigma);
        MPI_Barrier(MPI_COMM_WORLD);
        for(uint64_t i = 0; i < workload_args.chronicle_count; i++)
        {
            // create chronicle test
            std::string chronicle_name;
            if(workload_args.shared_story)
                chronicle_name = "chronicle_" + std::to_string(i);
            else
                chronicle_name = "chronicle_" + std::to_string(rank) + "_" + std::to_string(i);
            ret_i = createChronicleTimer.timeBlock(test_create_chronicle, client, chronicle_name);
            if(workload_args.barrier)
                MPI_Barrier(MPI_COMM_WORLD);

            uint64_t story_count_per_chronicle = workload_args.story_count / workload_args.chronicle_count;
            for(uint64_t j = 0; j < story_count_per_chronicle; j++)
            {
                // acquire story test
                std::string story_name;
                if(workload_args.shared_story)
                    story_name = "story_" + std::to_string(j);
                else
                    story_name = "story_" + std::to_string(rank) + "_" + std::to_string(j);
                auto ret = acquireStoryTimer.timeBlock(test_acquire_story, client, chronicle_name, story_name);
                ret_i = ret.first;
                story_handle = ret.second;
                if((workload_args.write || workload_args.latency_test) && ret_i != chronolog::CL_SUCCESS)
                {
                    std::cerr << "Failed to acquire story: " << story_name << " in Chronicle: " << chronicle_name
                              << ", ret: " << chronolog::to_string_client(ret_i) << std::endl;
                    exit(EXIT_FAILURE);
                }
                else if(workload_args.read && ret_i == chronolog::CL_ERR_NOT_EXIST)
                {
                    std::cerr << "Story does not exist: " << story_name << " in Chronicle: " << chronicle_name
                              << ", ret: " << chronolog::to_string_client(ret_i) << std::endl;
                    std::cout << "Please make sure a write test with the same configuration is executed before the "
                                 "read test."
                              << std::endl;
                    exit(EXIT_FAILURE);
                }
                if(workload_args.barrier)
                    MPI_Barrier(MPI_COMM_WORLD);

                if(workload_args.latency_test)
                {
                    // latency test: write events, then poll playback() until each
                    // becomes visible, recording send->visible latency per event.
                    uint64_t event_count_per_story = workload_args.event_count / workload_args.story_count;
                    size_t playback_n = workload_args.playback_n;
                    if(playback_n == 0)
                    {
                        // default: enough to cover this story's tail. For a shared
                        // story every rank writes into it, so scale by proc count.
                        playback_n = workload_args.shared_story ? static_cast<size_t>(event_count_per_story) * size
                                                                : static_cast<size_t>(event_count_per_story);
                    }
                    latency_measure_story(story_handle,
                                          client.client_id(),
                                          workload_args,
                                          event_count_per_story,
                                          playback_n,
                                          payload_str,
                                          latency_samples_ms,
                                          latency_logged,
                                          latency_seen);
                }
                else if(workload_args.read)
                {
                    // replay story test
                    uint64_t start_time = 0, end_time = UINT64_MAX;
                    std::vector<chronolog::Event> replay_events;
                    uint64_t event_count_per_story = workload_args.event_count / workload_args.story_count;
                    event_payload_size_per_rank = 0;
                    replayStoryTimer.timeBlock(
                            [&]()
                            {
                                for(uint64_t k = 0; k < event_count_per_story; k++)
                                {
                                    ret_u = test_replay_story(client,
                                                              chronicle_name,
                                                              story_name,
                                                              start_time,
                                                              end_time,
                                                              replay_events);
                                    replayStoryTimer.pauseTimer();
                                    for(const auto& event: replay_events)
                                    {
                                        event_payload_size_per_rank += event.log_record().size();
                                    }

                                    if(workload_args.event_interval > 0)
                                        usleep(workload_args.event_interval);
                                }
                            });
                }
                else
                {
                    // write event test
                    std::string event_payload;
                    writeEventTimer.timeBlock(
                            [&]()
                            {
                                uint64_t event_count_per_story = workload_args.event_count / workload_args.story_count;
                                for(uint64_t k = 0; k < event_count_per_story; k++)
                                {
                                    if(workload_args.event_payload_file.empty())
                                    {
                                        // randomly generate events size if range is specified
                                        writeEventTimer.pauseTimer();

                                        uint64_t event_size;
                                        if(workload_args.ave_event_size == workload_args.min_event_size &&
                                           workload_args.ave_event_size == workload_args.max_event_size)
                                        {
                                            event_size = workload_args.ave_event_size;
                                        }
                                        else
                                        {
                                            event_size = (unsigned long)std::min(
                                                    std::max(size_dist(gen),
                                                             (double)workload_args.min_event_size * 1.0),
                                                    (double)workload_args.max_event_size * 1.0);
                                        }
                                        event_payload = payload_str.substr(0, event_size);
                                        event_payload_size_per_rank += event_size;
                                        writeEventTimer.resumeTimer();
                                        ret_u = test_write_event(story_handle, event_payload);
                                        if(workload_args.barrier)
                                            MPI_Barrier(MPI_COMM_WORLD);

                                        if(workload_args.event_interval > 0)
                                            usleep(workload_args.event_interval);
                                    }
                                    else
                                    {
                                        // read event payload from payload file line by line
                                        std::ifstream input_file(workload_args.event_payload_file);

                                        // check if the file opened successfully
                                        if(input_file.is_open())
                                        {
                                            writeEventTimer.pauseTimer();
                                            uint64_t bigbang_timestamp = get_bigbang_timestamp(input_file);
                                            uint64_t last_event_timestamp = bigbang_timestamp;
                                            uint64_t event_timestamp;
                                            struct timespec sleep_ts
                                            {
                                            };
                                            while(std::getline(input_file, event_payload))
                                            {
                                                if(event_payload.empty())
                                                    continue;
                                                event_timestamp = get_event_timestamp(event_payload);
                                                if(event_timestamp < last_event_timestamp)
                                                {
                                                    LOG_INFO("An Out-of-Order event is found, sleeping for 1 second "
                                                             "...");
                                                    sleep_ts.tv_sec = 1;
                                                    sleep_ts.tv_nsec = 0;
                                                }
                                                else
                                                {
                                                    sleep_ts.tv_sec =
                                                            (long)(event_timestamp - last_event_timestamp) / 1000000000;
                                                    sleep_ts.tv_nsec =
                                                            (long)(event_timestamp - last_event_timestamp) % 1000000000;
                                                }
                                                // TODO: (Kun) work around on failure when daytime changes
                                                if(sleep_ts.tv_sec > 3600)
                                                    sleep_ts.tv_sec = 0;
                                                LOG_DEBUG("Sleeping for {}.{} seconds to emulate interval between "
                                                          "events ...",
                                                          sleep_ts.tv_sec,
                                                          sleep_ts.tv_nsec);
                                                nanosleep(&sleep_ts, nullptr);
                                                last_event_timestamp = event_timestamp;
                                                event_payload_size_per_rank += event_payload.size();
                                                writeEventTimer.resumeTimer();
                                                ret_u = test_write_event(story_handle, event_payload);
                                                if(workload_args.barrier)
                                                    MPI_Barrier(MPI_COMM_WORLD);
                                            }

                                            input_file.close();
                                        }
                                        else
                                        {
                                            std::cout << "Unable to open the file";
                                        }
                                    }
                                }
                            });
                }

                if(workload_args.barrier)
                    MPI_Barrier(MPI_COMM_WORLD);

                // release story test
                ret_i = releaseStoryTimer.timeBlock(test_release_story, client, chronicle_name, story_name);
                if(workload_args.barrier)
                    MPI_Barrier(MPI_COMM_WORLD);

                // destroy story test
                ret_i = destroyStoryTimer.timeBlock(test_destroy_story, client, chronicle_name, story_name);
                if(workload_args.barrier)
                    MPI_Barrier(MPI_COMM_WORLD);
            }

            // destroy chronicle test
            ret_i = destroyChronicleTimer.timeBlock(test_destroy_chronicle, client, chronicle_name);
            if(workload_args.barrier)
                MPI_Barrier(MPI_COMM_WORLD);
        }
    }
    double local_e2e_end = MPI_Wtime();

    ret_i = disconnectTimer.timeBlock(&chronolog::Client::Disconnect, client);
    assert(ret_i == chronolog::CL_SUCCESS);
    if(workload_args.barrier)
        MPI_Barrier(MPI_COMM_WORLD);

    if(workload_args.perf_test)
    {
        double local_e2e_duration = local_e2e_end - local_e2e_start;
        double global_e2e_start, global_e2e_end, global_e2e_duration_ave, global_e2e_duration;
        MPI_Reduce(&local_e2e_start, &global_e2e_start, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
        MPI_Reduce(&local_e2e_end, &global_e2e_end, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        global_e2e_duration = global_e2e_end - global_e2e_start;
        MPI_Reduce(&local_e2e_duration, &global_e2e_duration_ave, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

        uint64_t total_event_payload_size = 0;
        MPI_Reduce(&event_payload_size_per_rank,
                   &total_event_payload_size,
                   1,
                   MPI_UINT64_T,
                   MPI_SUM,
                   0,
                   MPI_COMM_WORLD);
        if(rank == 0)
        {
            std::cout << "==================================================================" << std::endl;
            std::cout << "=================      Performance results:      =================" << std::endl;
            std::cout << "==================================================================" << std::endl;
            global_e2e_duration_ave /= size;
            std::cout << "Total payload written: " << total_event_payload_size << " bytes" << std::endl;
            double connect_thpt, disconnect_thpt;
            double create_chronicle_thpt, destroy_chronicle_thpt;
            double acquire_story_thpt, release_story_thpt, destroy_story_thpt;
            double e2e_bandwidth, record_event_bw, record_event_thpt, replay_story_bw, replay_story_thpt;
            e2e_bandwidth = record_event_bw = record_event_thpt = replay_story_bw = replay_story_thpt = 0.0;
            if(workload_args.barrier)
            {
                connect_thpt = (double)size / connectTimer.getDuration();
                create_chronicle_thpt =
                        (double)workload_args.chronicle_count * size / createChronicleTimer.getDuration();
                acquire_story_thpt = (double)workload_args.story_count * size / acquireStoryTimer.getDuration();
                release_story_thpt = (double)workload_args.story_count * size / releaseStoryTimer.getDuration();
                destroy_story_thpt = (double)workload_args.story_count * size / destroyStoryTimer.getDuration();
                destroy_chronicle_thpt =
                        (double)workload_args.chronicle_count * size / destroyChronicleTimer.getDuration();
                disconnect_thpt = (double)size / disconnectTimer.getDuration();
                e2e_bandwidth = (double)total_event_payload_size / global_e2e_duration / 1e6;
                if(workload_args.read)
                {
                    replay_story_bw = (double)total_event_payload_size / replayStoryTimer.getDuration() / 1e6;
                    replay_story_thpt = (double)workload_args.event_count * size / replayStoryTimer.getDuration() / 1e6;
                }
                else
                {
                    record_event_bw = (double)total_event_payload_size / writeEventTimer.getDuration() / 1e6;
                    record_event_thpt = (double)workload_args.event_count * size / writeEventTimer.getDuration() / 1e6;
                }
            }
            else
            {
                connect_thpt = (double)size / connectTimer.getDurationAve();
                create_chronicle_thpt =
                        (double)workload_args.chronicle_count * size / createChronicleTimer.getDurationAve();
                acquire_story_thpt = (double)workload_args.story_count * size / acquireStoryTimer.getDurationAve();
                release_story_thpt = (double)workload_args.story_count * size / releaseStoryTimer.getDurationAve();
                destroy_story_thpt = (double)workload_args.story_count * size / destroyStoryTimer.getDurationAve();
                destroy_chronicle_thpt =
                        (double)workload_args.chronicle_count * size / destroyChronicleTimer.getDurationAve();
                disconnect_thpt = (double)size / disconnectTimer.getDurationAve();
                e2e_bandwidth = (double)total_event_payload_size / global_e2e_duration / 1e6;
                if(workload_args.read)
                {
                    replay_story_bw = (double)total_event_payload_size / replayStoryTimer.getDurationAve() / 1e6;
                    replay_story_thpt =
                            (double)workload_args.event_count * size / replayStoryTimer.getDurationAve() / 1e6;
                }
                else
                {
                    record_event_bw = (double)total_event_payload_size / writeEventTimer.getDurationAve() / 1e6;
                    record_event_thpt =
                            (double)workload_args.event_count * size / writeEventTimer.getDurationAve() / 1e6;
                }
            }
            std::cout << "Connect throughput: " << connect_thpt << " connections/s" << std::endl;
            std::cout << "CreateChronicle throughput: " << create_chronicle_thpt << " creations/s" << std::endl;
            std::cout << "AcquireStory throughput: " << acquire_story_thpt << " acquisitions/s" << std::endl;
            std::cout << "ReleaseStory throughput: " << release_story_thpt << " releases/s" << std::endl;
            std::cout << "DestroyStory throughput: " << destroy_story_thpt << " destructions/s" << std::endl;
            std::cout << "DestroyChronicle throughput: " << destroy_chronicle_thpt << " destructions/s" << std::endl;
            std::cout << "Disconnect throughput: " << disconnect_thpt << " disconnections/s" << std::endl;
            std::cout << "End-to-end (incl. metadata time) bandwidth: " << e2e_bandwidth << " MB/s" << std::endl;
            if(workload_args.read)
            {
                std::cout << "Replay-story (incl. metadata time) bandwidth: " << replay_story_bw << " MB/s"
                          << std::endl;
                std::cout << "Replay-story (incl. metadata time) throughput: " << replay_story_thpt << " events/s"
                          << std::endl;
            }
            else
            {
                std::cout << "Record-event (incl. metadata time) bandwidth: " << record_event_bw << " MB/s"
                          << std::endl;
                std::cout << "Record-event (incl. metadata time) throughput: " << record_event_thpt << " events/s"
                          << std::endl;
            }
        }
    }

    // Latency results are the whole point of latency mode, so always report them
    // (independent of -p). report_latency() runs MPI collectives on every rank.
    if(workload_args.latency_test)
    {
        report_latency(rank, size, latency_samples_ms, latency_logged, latency_seen);
    }

    MPI_Finalize();

    return 0;
}
