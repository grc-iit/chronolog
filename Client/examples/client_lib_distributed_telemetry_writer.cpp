#include <mpi.h>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>
#include <iomanip>
#include <getopt.h>
#include <margo.h>
#include <chronolog_client.h>

using namespace std;

typedef struct workload_conf_args_
{
    int duration_seconds = 60;
    int interval_seconds = 5;
} workload_conf_args;

void usage(char**argv)
{
    std::cerr << "\nUsage: " << argv[0] << " [options]\n"
                                           "-c|--config <config_file>\n"
                                           "-d|--duration <duration>\n"
                                           "-i|--interval <interval>\n" << argv[0] << std::endl;
}

std::pair <std::string, workload_conf_args> cmd_arg_parse(int argc, char**argv)
{
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    int opt;
    char *config_file = nullptr;
    workload_conf_args workload_args;

    // Define the long options and their corresponding short options
    struct option long_options[] = {{  "config"  , required_argument, nullptr, 'c'}
                                    , {"duration", optional_argument, nullptr, 'd'}
                                    , {"interval", optional_argument, nullptr, 'i'}
                                    , {nullptr   , 0                , nullptr, 0}};

    // Parse the command-line options
    while((opt = getopt_long(argc, argv, "c:d:i:", long_options, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                config_file = optarg;
                break;
            case 'd':
                workload_args.duration_seconds = std::stoi(optarg);
                break;
            case 'i':
                workload_args.interval_seconds = std::atoi(optarg);
                break;
            case '?':
                // Invalid option or missing argument
                usage(argv);
                exit(EXIT_FAILURE);
            default:
                // Unknown option
                std::cerr << "Unknown option: " << opt << std::endl;
                exit(EXIT_FAILURE);
        }
    }

    // Print config
    if(rank == 0)
    {
        std::cout << "[DistributedTelemetryWriter] Config file: " << (config_file ? config_file : "None") << std::endl;
        std::cout << "[DistributedTelemetryWriter] Duration: " << workload_args.duration_seconds << " seconds" << std::endl;
        std::cout << "[DistributedTelemetryWriter] Interval: " << workload_args.interval_seconds << " seconds" << std::endl;
    }

    // Return the config file and workload args
    if(config_file)
    {
        return {std::pair <std::string, workload_conf_args>((config_file), workload_args)};
    }
    else
    {
        return {"", workload_args};
    }
}

struct SystemMetrics
{
    double cpu_usage{};
    double memory_usage_mb{};
    long network_rx_bytes{};
    long network_tx_bytes{};
    std::chrono::steady_clock::time_point timestamp;
};

class TelemetryCollector
{
private:
    int rank{};
    int size{};
    std::string node_name;
    chronolog::Client *client{};

    // Story handles for different metrics - using traditional pointers
    chronolog::StoryHandle *cpu_story_handle;
    chronolog::StoryHandle *memory_story_handle;
    chronolog::StoryHandle *network_story_handle;

    // CPU monitoring - system-wide
    unsigned long long last_total_cpu_time{};
    unsigned long long last_idle_cpu_time{};

    // Network monitoring baseline
    long baseline_rx_bytes{};
    long baseline_tx_bytes{};

public:
    TelemetryCollector(int rank, int size): rank(rank), size(size)
    {
        // Get node name
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        node_name = std::string(hostname);

        // Initialize system-wide CPU monitoring
        getSystemCpuTimes(last_total_cpu_time, last_idle_cpu_time);

        // Get baseline network stats
        getNetworkStats(baseline_rx_bytes, baseline_tx_bytes);

        // Initialize story handles
        cpu_story_handle = nullptr;
        memory_story_handle = nullptr;
        network_story_handle = nullptr;
    }

    bool initializeChronoLog(chronolog::ClientConfiguration &confManager)
    {
        // Configure the client connection
        chronolog::ClientPortalServiceConf portalConf = confManager.PORTAL_CONF;

        // Create a ChronoLog client
        client = new chronolog::Client(portalConf);

        // Connect to ChronoVisor
        int ret = client->Connect();
        assert(ret == chronolog::CL_SUCCESS);

        // Create a chronicle named after the hostname
        std::string chronicle_name = node_name;
        std::map <std::string, std::string> chronicle_attrs;
        chronicle_attrs["description"] = "System telemetry metrics for node " + node_name;
        chronicle_attrs["node"] = node_name;
        int flags = 0;
        ret = client->CreateChronicle(chronicle_name, chronicle_attrs, flags);
        assert(ret == chronolog::CL_SUCCESS || ret == chronolog::CL_ERR_CHRONICLE_EXISTS);

        // Acquire stories for different metrics
        std::map <std::string, std::string> story_attrs;
        story_attrs["node"] = node_name;
        story_attrs["rank"] = std::to_string(rank);

        // CPU usage story
        auto cpu_result = client->AcquireStory(chronicle_name, "cpu_usage", story_attrs, flags);
        assert(cpu_result.first == chronolog::CL_SUCCESS);
        cpu_story_handle = cpu_result.second;

        // Memory usage story
        auto memory_result = client->AcquireStory(chronicle_name, "memory_usage", story_attrs, flags);
        assert(memory_result.first == chronolog::CL_SUCCESS);
        memory_story_handle = memory_result.second;

        // Network usage story
        auto network_result = client->AcquireStory(chronicle_name, "network_usage", story_attrs, flags);
        assert(network_result.first == chronolog::CL_SUCCESS);
        network_story_handle = network_result.second;

        return true;
    }

    void cleanup()
    {
        if(client)
        {
            std::string chronicle_name = node_name;

            // Release all stories
            if(cpu_story_handle)
            {
                client->ReleaseStory(chronicle_name, "cpu_usage");
                cpu_story_handle = nullptr;
            }
            if(memory_story_handle)
            {
                client->ReleaseStory(chronicle_name, "memory_usage");
                memory_story_handle = nullptr;
            }
            if(network_story_handle)
            {
                client->ReleaseStory(chronicle_name, "network_usage");
                network_story_handle = nullptr;
            }

            // Disconnect from ChronoVisor
            client->Disconnect();
            delete client;
        }
    }

    void getSystemCpuTimes(unsigned long long &total_time, unsigned long long &idle_time)
    {
        std::ifstream stat_file("/proc/stat");
        std::string line;
        std::getline(stat_file, line);

        std::istringstream iss(line);
        std::string cpu_label;
        unsigned long long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;

        iss >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal >> guest >> guest_nice;

        // Total CPU time = user + nice + system + idle + iowait + irq + softirq + steal
        total_time = user + nice + system + idle + iowait + irq + softirq + steal;
        idle_time = idle + iowait;
    }

    double getSystemCpuUsage()
    {
        unsigned long long current_total_time, current_idle_time;
        getSystemCpuTimes(current_total_time, current_idle_time);

        unsigned long long total_diff = current_total_time - last_total_cpu_time;
        unsigned long long idle_diff = current_idle_time - last_idle_cpu_time;

        double cpu_usage = 0.0;
        if(total_diff > 0)
        {
            cpu_usage = 100.0 * (1.0 - (double)idle_diff / (double)total_diff);
        }

        last_total_cpu_time = current_total_time;
        last_idle_cpu_time = current_idle_time;

        return cpu_usage;
    }

    double getSystemMemoryUsage()
    {
        std::ifstream meminfo_file("/proc/meminfo");
        std::string line;
        unsigned long long mem_total = 0, mem_free = 0, buffers = 0, cached = 0, sreclaim = 0;

        while(std::getline(meminfo_file, line))
        {
            std::istringstream iss(line);
            std::string label;
            unsigned long long value;
            std::string unit;

            iss >> label >> value >> unit;

            if(label == "MemTotal:")
            {
                mem_total = value;
            }
            else if(label == "MemFree:")
            {
                mem_free = value;
            }
            else if(label == "Buffers:")
            {
                buffers = value;
            }
            else if(label == "Cached:")
            {
                cached = value;
            }
            else if(label == "SReclaimable:")
            {
                sreclaim = value;
            }
        }

        // Available memory = MemFree + Buffers + Cached + SReclaimable
        unsigned long long mem_available = mem_free + buffers + cached + sreclaim;
        unsigned long long mem_used = mem_total - mem_available;

        return (double)mem_used / 1024.0; // Convert from KB to MB
    }

    void getNetworkStats(long &rx_bytes, long &tx_bytes)
    {
        std::ifstream net_dev("/proc/net/dev");
        std::string line;
        rx_bytes = 0;
        tx_bytes = 0;

        // Skip header lines
        std::getline(net_dev, line);
        std::getline(net_dev, line);

        while(std::getline(net_dev, line))
        {
            if(line.find("lo:") != std::string::npos)
            {
                continue; // Skip loopback interface
            }

            std::istringstream iss(line);
            std::string interface;
            long rx, tx;
            std::string dummy;

            iss >> interface >> rx;
            // Skip 7 more rx fields
            for(int i = 0; i < 7; i++) iss >> dummy;
            iss >> tx;

            rx_bytes += rx;
            tx_bytes += tx;
        }
    }

    SystemMetrics collectMetrics()
    {
        SystemMetrics metrics;
        metrics.timestamp = std::chrono::steady_clock::now();
        metrics.cpu_usage = getSystemCpuUsage();
        metrics.memory_usage_mb = getSystemMemoryUsage();

        long current_rx, current_tx;
        getNetworkStats(current_rx, current_tx);
        metrics.network_rx_bytes = current_rx - baseline_rx_bytes;
        metrics.network_tx_bytes = current_tx - baseline_tx_bytes;

        return metrics;
    }

    void logMetrics(const SystemMetrics &metrics)
    {
        auto time_since_epoch = metrics.timestamp.time_since_epoch();
        auto millis = std::chrono::duration_cast <std::chrono::milliseconds>(time_since_epoch).count();

        // Log CPU usage to cpu_usage story
        std::ostringstream cpu_entry;
        cpu_entry << "{" << "\"timestamp\":" << millis << R"(,"node":")" << node_name << "\"" << ",\"rank\":" << rank
                  << ",\"cpu_usage\":" << std::fixed << std::setprecision(2) << metrics.cpu_usage << "}";
        cpu_story_handle->log_event(std::to_string(metrics.cpu_usage));

        // Log memory usage to memory_usage story
        std::ostringstream memory_entry;
        memory_entry << "{" << "\"timestamp\":" << millis << R"(,"node":")" << node_name << "\"" << ",\"rank\":" << rank
                     << ",\"memory_mb\":" << std::fixed << std::setprecision(2) << metrics.memory_usage_mb << "}";
        memory_story_handle->log_event(std::to_string(metrics.memory_usage_mb));

        // Log network usage to network_usage story
        std::ostringstream network_entry;
        network_entry << "{" << "\"timestamp\":" << millis << R"(,"node":")" << node_name << "\"" << ",\"rank\":"
                      << rank << ",\"network_rx_bytes\":" << metrics.network_rx_bytes << ",\"network_tx_bytes\":"
                      << metrics.network_tx_bytes << "}";
        network_story_handle->log_event(std::to_string(metrics.network_rx_bytes) + "," +
                                        std::to_string(metrics.network_tx_bytes));
    }

    void run(int duration_seconds, int interval_seconds)
    {
        auto start_time = std::chrono::steady_clock::now();
        auto end_time = start_time + std::chrono::seconds(duration_seconds);

        if(rank == 0)
        {
            std::cout << "[DistributedTelemetryWriter] Starting telemetry collection for " << duration_seconds << " seconds with "
                      << interval_seconds << "s intervals\n";
            std::cout << "[DistributedTelemetryWriter] Chronicle: " << node_name << "\n";
            std::cout << "[DistributedTelemetryWriter] Stories: cpu_usage, memory_usage, network_usage\n";
        }

        while(std::chrono::steady_clock::now() < end_time)
        {
            SystemMetrics metrics = collectMetrics();
            logMetrics(metrics);

            // MPI barrier to synchronize all processes
            MPI_Barrier(MPI_COMM_WORLD);

            if(rank == 0)
            {
                auto elapsed = std::chrono::duration_cast <std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start_time).count();
                std::cout << "[DistributedTelemetryWriter] Collected metrics at " << elapsed << "s - "
                          << "CPU: " << std::fixed << std::setprecision(1) << metrics.cpu_usage << "%, "
                          << "Memory: " << std::setprecision(1) << metrics.memory_usage_mb << "MB, "
                          << "Network RX: " << metrics.network_rx_bytes << " bytes, "
                          << "Network TX: " << metrics.network_tx_bytes << " bytes" << std::endl;
            }

            std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
        }

        if(rank == 0)
        {
            std::cout << "[DistributedTelemetryWriter] Telemetry collection completed\n";
        }
    }
};

int main(int argc, char *argv[])
{
    // To suppress argobots warning
    std::string argobots_conf_str = R"({"argobots" : {"abt_mem_max_num_stacks" : 8
                                                    , "abt_thread_stacksize" : 2097152}})";
    margo_set_environment(argobots_conf_str.c_str());

    // Initialize MPI
    MPI_Init(&argc, &argv);

    int rank, size;
    rank = 0;
    size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    std::string default_conf_file_path = "./default_client_conf.json";
    std::pair <std::string, workload_conf_args> cmd_args = cmd_arg_parse(argc, argv);
    std::string conf_file_path = cmd_args.first;
    workload_conf_args workload_args = cmd_args.second;
    if(conf_file_path.empty())
    {
        conf_file_path = default_conf_file_path;
    }

    chronolog::ClientConfiguration confManager;
    if(!conf_file_path.empty())
    {
        if(!confManager.load_from_file(conf_file_path))
        {
            std::cerr << "[DistributedTelemetryWriter] Failed to load configuration file '" << conf_file_path
                      << "'. Using default values instead." << std::endl;
        }
        else
        {
            std::cout << "[DistributedTelemetryWriter] Configuration file loaded successfully from '" << conf_file_path << "'."
                      << std::endl;
        }
    }
    else
    {
        std::cout << "[DistributedTelemetryWriter] No configuration file provided. Using default values." << std::endl;
    }

    if(rank == 0)
    {
        confManager.log_configuration(std::cout);
    }

    std::cout << "[DistributedTelemetryWriter] Monitoring system stats on " << rank << "-th node out of "
                                                                            << size << " nodes every "
                                                                            << workload_args.interval_seconds
                                                                            << " seconds for "
                                                                            << workload_args.duration_seconds
                                                                            << " seconds" << std::endl;

    TelemetryCollector collector(rank, size);

    // Initialize ChronoLog client
    bool chronolog_available = collector.initializeChronoLog(confManager);
    if(rank == 0)
    {
        if(chronolog_available)
        {
            std::cout << "[DistributedTelemetryWriter] ChronoLog initialized successfully\n";
        }
        else
        {
            std::cout << "Error: ChronoLog initialization failed\n";
            MPI_Finalize();
            return -1;
        }
    }

    // Run telemetry collection
    collector.run(workload_args.duration_seconds, workload_args.interval_seconds);

    // Cleanup
    collector.cleanup();

    MPI_Finalize();
    return 0;
}