#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <ClientConfiguration.h>
#include <chronolog_client.h>

using namespace std::chrono_literals;
namespace chl = chronolog;

static volatile std::sig_atomic_t g_running = 1;
static void handle_sig(int) { g_running = 0; }

static std::string get_hostname()
{
    char buf[256];
    buf[0] = '\0';
    if(gethostname(buf, sizeof(buf)) != 0)
        return "unknown-host";
    buf[sizeof(buf) - 1] = '\0';
    return std::string(buf);
}

struct Args
{
    std::string config;     // -c
    std::string chronicle;  // -r
    int duration_sec = 300; // -d
    int interval_sec = 5;   // -i
};

static void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0
              << " -c <config.json> [options]\n"
                 " -c, --config <path>      ChronoLog client config JSON\n"
                 " -r, --chronicle <name>   Chronicle name\n"
                 " -d, --duration <sec>     Duration (default: 300)\n"
                 " -i, --interval <sec>     Interval (default: 5)\n";
}

static Args parse_args(int argc, char** argv)
{
    Args a;
    static struct option long_opts[] = {{"config", required_argument, nullptr, 'c'},
                                        {"chronicle", required_argument, nullptr, 'r'},
                                        {"duration", required_argument, nullptr, 'd'},
                                        {"interval", required_argument, nullptr, 'i'},
                                        {nullptr, 0, nullptr, 0}};
    int c, idx = 0;
    while((c = getopt_long(argc, argv, "c:r:d:i:", long_opts, &idx)) != -1)
    {
        switch(c)
        {
            case 'c':
                a.config = optarg;
                break;
            case 'r':
                a.chronicle = optarg;
                break;
            case 'd':
                a.duration_sec = std::max(1, atoi(optarg));
                break;
            case 'i':
                a.interval_sec = std::max(1, atoi(optarg));
                break;
            default:
                usage(argv[0]);
                std::exit(2);
        }
    }
    if(a.chronicle.empty())
        a.chronicle = get_hostname();
    if(a.config.empty())
    {
        usage(argv[0]);
        std::exit(2);
    }
    return a;
}

// Get the metrics
struct CpuTracker
{
    unsigned long long last_total = 0, last_idle = 0;
    static void read_totals(unsigned long long& total, unsigned long long& idle)
    {
        std::ifstream f("/proc/stat");
        std::string line;
        std::getline(f, line);
        std::istringstream iss(line);
        std::string lbl;
        unsigned long long user, nice, sys, idle_t, iowait, irq, sirq, steal, guest, gnice;
        iss >> lbl >> user >> nice >> sys >> idle_t >> iowait >> irq >> sirq >> steal >> guest >> gnice;
        total = user + nice + sys + idle_t + iowait + irq + sirq + steal;
        idle = idle_t + iowait;
    }
    double usage_pct()
    {
        unsigned long long t, i;
        read_totals(t, i);
        unsigned long long dt = t - last_total;
        unsigned long long di = i - last_idle;
        last_total = t;
        last_idle = i;
        if(dt == 0)
            return 0.0;
        double busy = 100.0 * (1.0 - (double)di / (double)dt);
        if(busy < 0)
            busy = 0;
        if(busy > 100)
            busy = 100;
        return busy;
    }
};

static double mem_used_mb()
{
    std::ifstream f("/proc/meminfo");
    std::string key, unit;
    unsigned long long val = 0;
    unsigned long long mem_total = 0, mem_free = 0, buffers = 0, cached = 0, sreclaim = 0;
    while(f >> key >> val >> unit)
    {
        if(key == "MemTotal:")
            mem_total = val;
        else if(key == "MemFree:")
            mem_free = val;
        else if(key == "Buffers:")
            buffers = val;
        else if(key == "Cached:")
            cached = val;
        else if(key == "SReclaimable:")
            sreclaim = val;
    }
    unsigned long long avail = mem_free + buffers + cached + sreclaim;
    unsigned long long used_kb = (mem_total > avail) ? (mem_total - avail) : 0;
    return used_kb / 1024.0;
}

static void net_totals(long& rx, long& tx)
{
    std::ifstream f("/proc/net/dev");
    std::string line;
    rx = 0;
    tx = 0;
    std::getline(f, line);
    std::getline(f, line);
    while(std::getline(f, line))
    {
        if(line.find("lo:") != std::string::npos)
            continue;
        std::istringstream iss(line);
        std::string ifname;
        long r;
        std::string dummy;
        iss >> ifname >> r;
        for(int i = 0; i < 7; i++) iss >> dummy;
        long t;
        iss >> t;
        rx += r;
        tx += t;
    }
}

static chronolog::StoryHandle* acquire_or_create_story(chronolog::Client& client,
                                                       const std::string& chronicle,
                                                       const std::string& story,
                                                       const std::map<std::string, std::string>& create_attrs)
{
    // Acquire story with empty attributes
    std::map<std::string, std::string> empty_attrs;
    int f_open = 0;
    auto r_open1 = client.AcquireStory(chronicle, story, empty_attrs, f_open);
    if(r_open1.first == chronolog::CL_SUCCESS && r_open1.second)
        return r_open1.second;

    // Create the story with the attributes
    int f_create = 1;
    auto r_create = client.AcquireStory(chronicle, story, create_attrs, f_create);
    if(r_create.first == chronolog::CL_SUCCESS && r_create.second)
        return r_create.second;

    auto r_open2 = client.AcquireStory(chronicle, story, empty_attrs, f_open);
    if(r_open2.first == chronolog::CL_SUCCESS && r_open2.second)
        return r_open2.second;

    std::vector<std::string> ss;
    client.ShowStories(chronicle, ss);
    std::cerr << "[writer] AcquireStory(" << story << ") failed\n"
              << " open1 rc=" << r_open1.first << " (" << chl::to_string_client(r_open1.first) << ")\n"
              << " create rc=" << r_create.first << " (" << chl::to_string_client(r_create.first) << ")\n"
              << " open2 rc=" << r_open2.first << " (" << chl::to_string_client(r_open2.first) << ")\n"
              << " stories under " << chronicle << ":";
    for(auto& s: ss) std::cerr << " " << s;
    std::cerr << "\n";
    return nullptr;
}

int main(int argc, char** argv)
{
    std::signal(SIGINT, handle_sig);
    std::signal(SIGTERM, handle_sig);

    Args args = parse_args(argc, argv);
    std::cout << "[writer] config=" << args.config << " chronicle=" << args.chronicle
              << " duration=" << args.duration_sec << "s" << " interval=" << args.interval_sec << "s";

    // Load client config
    chronolog::ClientConfiguration cfg;
    if(!cfg.load_from_file(args.config))
    {
        std::cerr << "[writer] failed to load config: " << args.config << "\n";
        return 2;
    }

    // Build portal conf and connect
    chronolog::ClientPortalServiceConf portalConf = cfg.PORTAL_CONF;
    chronolog::Client client(portalConf);

    int rc = client.Connect();
    if(rc != chronolog::CL_SUCCESS)
    {
        std::cerr << "[writer] Connect failed rc=" << rc << "\n";
        return 3;
    }

    {
        std::vector<std::string> chs;
        client.ShowChronicles(chs);
        std::cerr << "[debug] chronicles:";
        for(auto& c: chs) std::cerr << " " << c;
        std::cerr << "\n";
    }


    // Create chronicle, ignore if it exists already
    std::map<std::string, std::string> chron_attrs{{"description", "telemetry for " + args.chronicle},
                                                   {"node", args.chronicle}};
    int flags = 1;
    rc = client.CreateChronicle(args.chronicle, chron_attrs, flags);
    if(rc != chronolog::CL_SUCCESS && rc != chronolog::CL_ERR_CHRONICLE_EXISTS)
    {
        std::cerr << "[writer] CreateChronicle failed rc=" << rc << "\n";
        client.Disconnect();
        return 4;
    }

    {
        std::vector<std::string> ss;
        client.ShowStories(args.chronicle, ss);
        std::cerr << "[debug] stories under " << args.chronicle << ":";
        for(auto& s: ss) std::cerr << " " << s;
        std::cerr << "\n";
    }


    // Acquie/create three stories
    std::map<std::string, std::string> story_attrs{{"node", args.chronicle}, {"source", "client_writer"}};
    chronolog::StoryHandle* h_cpu = acquire_or_create_story(client, args.chronicle, "cpu_usage", story_attrs);
    chronolog::StoryHandle* h_mem = acquire_or_create_story(client, args.chronicle, "memory_usage", story_attrs);
    chronolog::StoryHandle* h_net = acquire_or_create_story(client, args.chronicle, "network_usage", story_attrs);
    if(!h_cpu || !h_mem || !h_net)
    {
        std::cerr << "[writer] failed to acquire one or more stories, aborting\n";
        client.Disconnect();
        return 5;
    }

    // Prepare metric sources
    CpuTracker cpu;
    (void)cpu.usage_pct();
    long rx0 = 0, tx0 = 0;
    net_totals(rx0, tx0);

    // Start the loop as per the time from the flags to write metric stories for that long
    auto start = std::chrono::steady_clock::now();
    auto until = start + std::chrono::seconds(args.duration_sec);

    while(g_running && std::chrono::steady_clock::now() < until)
    {
        double cpu_pct = 0.0, mem_mb = 0.0;
        long rx_cum = 0, tx_cum = 0;

        cpu_pct = cpu.usage_pct();
        mem_mb = mem_used_mb();
        long rx, tx;
        net_totals(rx, tx);
        rx_cum = (rx - rx0);
        tx_cum = (tx - tx0);
        if(rx_cum < 0)
            rx_cum = 0;
        if(tx_cum < 0)
            tx_cum = 0;

        h_cpu->log_event(std::to_string(cpu_pct));
        h_mem->log_event(std::to_string(mem_mb));
        h_net->log_event(std::to_string(rx_cum) + "," + std::to_string(tx_cum));

        std::cout << "[writer] cpu=" << std::fixed << std::setprecision(1) << cpu_pct
                  << "% mem=" << std::setprecision(1) << mem_mb << "MB" << " net=" << rx_cum << "," << tx_cum
                  << " bytes\n";

        for(int i = 0; i < args.interval_sec && g_running; ++i) std::this_thread::sleep_for(1s);
    }

    // cleanup
    client.ReleaseStory(args.chronicle, "cpu_usage");
    client.ReleaseStory(args.chronicle, "memory_usage");
    client.ReleaseStory(args.chronicle, "network_usage");
    client.Disconnect();
    return 0;
}
