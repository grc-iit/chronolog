#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <ClientConfiguration.h>
#include <chronolog_client.h>

#include "InfluxDBSink.h"
#include "StreamSink.h"
#include "Transform.h"

using namespace std::chrono_literals;

static volatile std::sig_atomic_t g_running = 1;
static void handle_signal(int) { g_running = 0; }

struct Args
{
    std::string config_path;
    std::string chronicle;
    std::vector<std::string> stories;
    uint64_t window_sec = 300;
    uint64_t poll_interval_sec = 5;
    std::string influx_url;
    std::string influx_token;
};

static void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0
              << " [options]\n"
                 " -c, --config <file>          ChronoLog client config JSON\n"
                 " -r, --chronicle <name>       Chronicle name\n"
                 " -s, --stories <comma-list>   Stories to stream\n"
                 " --window-sec <N>             Initial backfill window in seconds (default 300)\n"
                 " --poll-interval-sec <N>      Poll interval in seconds (default 5)\n"
                 " --influx-url <url>           Influx write URL\n"
                 " --influx-token <token>       Influx API token\n";
}

static std::vector<std::string> split_commas(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while(start < s.size())
    {
        size_t pos = s.find(',', start);
        if(pos == std::string::npos)
            pos = s.size();
        std::string tok = s.substr(start, pos - start);
        size_t b = tok.find_first_not_of(" \t\r\n");
        size_t e = tok.find_last_not_of(" \t\r\n");
        if(b != std::string::npos)
            out.emplace_back(tok.substr(b, e - b + 1));
        start = pos + 1;
    }
    return out;
}

static Args parse_args(int argc, char** argv)
{
    Args a;
    static struct option long_opts[] = {{"config", required_argument, nullptr, 'c'},
                                        {"chronicle", required_argument, nullptr, 'r'},
                                        {"stories", required_argument, nullptr, 's'},
                                        {"window-sec", required_argument, nullptr, 1},
                                        {"poll-interval-sec", required_argument, nullptr, 2},
                                        {"influx-url", required_argument, nullptr, 3},
                                        {"influx-token", required_argument, nullptr, 4},
                                        {nullptr, 0, nullptr, 0}};

    while(true)
    {
        int idx = 0;
        int c = getopt_long(argc, argv, "c:r:s:", long_opts, &idx);
        if(c == -1)
            break;
        switch(c)
        {
            case 'c':
                a.config_path = optarg;
                break;
            case 'r':
                a.chronicle = optarg;
                break;
            case 's':
                a.stories = split_commas(optarg);
                break;
            case 1:
                a.window_sec = std::stoull(optarg);
                break;
            case 2:
                a.poll_interval_sec = std::stoull(optarg);
                break;
            case 3:
                a.influx_url = optarg;
                std::cout << "[client_reader_stream] Influx URL from command line: " << a.influx_url << "\n";
                break;
            case 4:
                a.influx_token = optarg;
                std::cout << "[client_reader_stream] Influx token from command line: " << a.influx_token << "\n";
                break;
            default:
                usage(argv[0]);
                std::exit(2);
        }
    }

    if(a.influx_token.empty())
    {
        const char* t = std::getenv("INFLUX_TOKEN");
        if(t)
        {
            a.influx_token = t;
            std::cout << "[client_reader_stream] Influx token from ENV: " << a.influx_token << "\n";
        }
        else
        {
            std::cerr << "[client_reader_stream] Influx token not provided, exiting ...\n";
            usage(argv[0]);
            std::exit(2);
        }
    }
    return a;
}

static inline uint64_t now_ns()
{
    using namespace std::chrono;
    return high_resolution_clock::now().time_since_epoch().count();
}

static void
log_replay_error(int rc, const std::string& chronicle, const std::string& story, uint64_t start_ns, uint64_t end_ns)
{
    const uint64_t dur_s = (end_ns > start_ns) ? (end_ns - start_ns) / 1000000000ULL : 0ULL;

    if(rc == -12) // CL_ERR_QUERY_TIMED_OUT
    {
        std::cerr << "[client_reader_stream] Replay timed out for " << chronicle << "/" << story << " window=["
                  << start_ns << "," << end_ns << "] (~" << dur_s << "s)"
                  << " Will be retried automatically\n";
    }
    else
    {
        std::cerr << "[client_reader_stream] Replay failed for " << chronicle << "/" << story << " rc=" << rc
                  << " window=[" << start_ns << "," << end_ns << "] (~" << dur_s << "s)" << "Retrying\n";
    }
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    Args args = parse_args(argc, argv);

    if(args.config_path.empty() || args.chronicle.empty() || args.stories.empty() || args.influx_url.empty() ||
       args.influx_token.empty())
    {
        usage(argv[0]);
        return 2;
    }

    chronolog::ClientConfiguration confManager;
    if(!args.config_path.empty())
    {
        if(!confManager.load_from_file(args.config_path))
        {
            std::cerr << "[client_reader_stream] Failed to load config '" << args.config_path << std::endl;
        }
        else
        {
            std::cout << "[client_reader_stream] Config loaded from '" << args.config_path << "'" << std::endl;
        }
    }
    confManager.log_configuration(std::cout);

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

    chronolog::Client client(portalConf, queryConf);
    int rc = client.Connect();
    if(rc != chronolog::CL_SUCCESS)
    {
        std::cerr << "[client_reader_stream] Connect failed rc=" << rc << "\n";
        return 1;
    }

    int flags = 1;
    std::map<std::string, std::string> chronicle_attrs;
    std::map<std::string, std::string> story_attrs;

    // rc = client.CreateChronicle(args.chronicle, chronicle_attrs, flags);

    for(const auto& s: args.stories)
    {
        auto acq = client.AcquireStory(args.chronicle, s, story_attrs, flags);
        (void)acq;
    }

    // Influx sink settings
    InfluxDBSink::Options io;
    io.url = args.influx_url;
    io.token = args.influx_token;
    io.max_batch_points = 5000;
    io.timeout_ms = 5000;
    io.max_retries = 3;

    InfluxDBSink sink(io);

    std::map<std::string, uint64_t> last_ts_ns;
    const uint64_t now0 = now_ns();
    for(const auto& s: args.stories) { last_ts_ns[s] = now0 - args.window_sec * 1000000000ULL; }

    std::cout << "[client_reader_stream] Chronicle=" << args.chronicle << " Stories=(";
    for(size_t i = 0; i < args.stories.size(); ++i)
    {
        std::cout << args.stories[i] << (i + 1 < args.stories.size() ? "," : "");
    }
    std::cout << ") window_sec=" << args.window_sec << " poll_interval_sec=" << args.poll_interval_sec << "\n";

    // Run in a loop until terminated manually
    while(g_running)
    {
        const uint64_t t_end = now_ns();

        for(const auto& story: args.stories)
        {
            const uint64_t t_start = last_ts_ns[story];

            std::vector<chronolog::Event> events;
            int ret = client.ReplayStory(args.chronicle, story, t_start, t_end, events);
            if(ret != chronolog::CL_SUCCESS)
            {
                log_replay_error(ret, args.chronicle, story, t_start, t_end);
            }
            else if(!events.empty())
            {

                TelemetryBatch batch = toTelemetry(events, args.chronicle, story);
                if(!sink.send(batch))
                {
                    std::cerr << "[client_reader_stream] Influx sink send failed for " << args.chronicle << "/" << story
                              << " (batch=" << batch.points.size() << ")\n";
                }
                else
                {
                    std::cout << "[client_reader_stream] Sent " << batch.points.size() << " points to Influx ("
                              << args.chronicle << "/" << story << ")\n";
                }
            }

            last_ts_ns[story] = t_end + 1;
        }

        for(uint64_t i = 0; i < args.poll_interval_sec && g_running; ++i) { std::this_thread::sleep_for(1s); }
    }

    // cleanup
    for(const auto& s: args.stories) { client.ReleaseStory(args.chronicle, s); }
    client.Disconnect();
    return 0;
}
