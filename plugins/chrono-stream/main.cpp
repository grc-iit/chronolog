#include "chronolog/stream/stream.h"
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <set>
#include <thread>
#include <unistd.h>
namespace
{
volatile std::sig_atomic_t stopping{};
void stop(int) { stopping = 1; }
std::string env(const char* name, const std::string& fallback)
{
    auto value = std::getenv(name);
    return value ? value : fallback;
}
void pause(std::chrono::milliseconds time)
{
    auto until = std::chrono::steady_clock::now() + time;
    while(!stopping && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
}
} // namespace
int main(int argc, char** argv)
{
    using namespace chronolog;
    try
    {
        std::map<std::string, std::string> args;
        std::set<std::string> allowed = {"--visor",
                                         "--player",
                                         "--chronicle",
                                         "--host",
                                         "--interval-ms",
                                         "--samples",
                                         "--duration-ms",
                                         "--proc-root",
                                         "--durability",
                                         "--influx-url",
                                         "--org",
                                         "--bucket",
                                         "--token",
                                         "--consumer",
                                         "--batch-count",
                                         "--batch-age-ms",
                                         "--stories"};
        bool loopback = false;
        for(int i = 1; i < argc; ++i)
        {
            std::string key = argv[i];
            if(key == "--help")
            {
                std::cout << "ChronoLog telemetry: --visor HOST:PORT --player HOST:PORT --chronicle NAME --duration-ms "
                             "N\ncollector: --host NAME --interval-ms N --samples N --include-loopback --proc-root "
                             "PATH --durability durable|accepted\nexporter: --influx-url URL --org NAME --bucket NAME "
                             "--token TOKEN --consumer NAME --batch-count N --batch-age-ms N --stories CSV\n";
                return 0;
            }
            if(key == "--include-loopback")
            {
                loopback = true;
                continue;
            }
            if(!allowed.contains(key) || i + 1 >= argc)
                throw std::invalid_argument("invalid option " + key);
            args[key] = argv[++i];
        }
        auto get = [&](const std::string& name, const std::string& fallback)
        { return args.contains(name) ? args.at(name) : fallback; };
        auto number = [&](const std::string& name, uint64_t fallback, uint64_t maximum)
        {
            auto value = get(name, std::to_string(fallback));
            size_t used{};
            auto n = std::stoull(value, &used);
            if(used != value.size() || value.front() == '-' || n > maximum)
                throw std::invalid_argument("invalid " + name);
            return n;
        };
        client::ClientOptions options;
        options.catalog_endpoint = get("--visor", env("CHRONOLOG_VISOR", "127.0.0.1:50051"));
        options.player_endpoint = get("--player", env("CHRONOLOG_PLAYER", "127.0.0.1:50054"));
        options.rpc_timeout = std::chrono::seconds(3);
        auto client = client::Client::Connect(options);
        if(!client.ok())
        {
            std::cerr << client.status() << '\n';
            return 1;
        }
        auto chronicle = get("--chronicle", env("CHRONOLOG_STREAM_CHRONICLE", "host-metrics"));
        auto duration = number("--duration-ms", 0, 86400000);
        client::Deadline deadline;
        if(duration)
            deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(duration);
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
#ifdef STREAM_COLLECT
        char hostname[256]{};
        if(gethostname(hostname, sizeof(hostname) - 1) != 0)
            throw std::runtime_error("gethostname failed");
        auto root = get("--proc-root", "/proc");
        stream::Collector collector(*client,
                                    chronicle,
                                    get("--host", hostname),
                                    {root + "/stat", root + "/meminfo", root + "/net/dev"},
                                    loopback);
        auto interval = number("--interval-ms", 1000, 60000);
        if(!interval)
            throw std::invalid_argument("interval must be positive");
        auto samples = number("--samples", 0, 1000000);
        auto durability = get("--durability", "durable");
        if(durability != "durable" && durability != "accepted")
            throw std::invalid_argument("invalid durability");
        for(uint64_t n = 0;
            !stopping && (!samples || n < samples) && (!deadline || std::chrono::system_clock::now() < *deadline);
            ++n)
        {
            auto result = collector.sample(durability == "durable" ? Durability::Durable : Durability::Accepted,
                                           {},
                                           deadline);
            if(!result.ok())
            {
                std::cerr << result.status() << '\n';
                return 1;
            }
            for(const auto& r: *result)
            {
                if(!r.ok())
                {
                    std::cerr << r.status() << '\n';
                    return 1;
                }
                std::cout << "sample " << n << " acked=" << r->acked() << "\n";
            }
            std::cout.flush();
            if(!samples || n + 1 < samples)
                pause(std::chrono::milliseconds(interval));
        }
#else
        (void)loopback;
        stream::SinkOptions sinkOptions;
        sinkOptions.url = get("--influx-url", env("INFLUX_URL", "http://127.0.0.1:8086"));
        sinkOptions.org = get("--org", env("INFLUX_ORG", "chronolog"));
        sinkOptions.bucket = get("--bucket", env("INFLUX_BUCKET", "telemetry"));
        sinkOptions.token = get("--token", env("INFLUX_TOKEN", ""));
        stream::InfluxSink sink(sinkOptions);
        kvs::Store positions(*client, chronicle + ".export-positions");
        stream::ExportOptions exportOptions;
        exportOptions.consumer = get("--consumer", "influx");
        exportOptions.batch_count = number("--batch-count", 128, 10000);
        exportOptions.batch_age = std::chrono::milliseconds(number("--batch-age-ms", 250, 30000));
        std::vector<std::string> stories;
        std::string list = get("--stories", "system.cpu,system.memory,system.network");
        size_t at = 0;
        do {
            auto comma = list.find(',', at);
            stories.push_back(list.substr(at, comma - at));
            if(comma == std::string::npos)
                break;
            at = comma + 1;
        } while(stories.size() < 65);
        auto readyUntil = std::chrono::system_clock::now() + std::chrono::seconds(30);
        if(deadline)
            readyUntil = std::min(readyUntil, *deadline);
        for(const auto& story: stories)
        {
            bool ready = false;
            while(!stopping && std::chrono::system_clock::now() < readyUntil)
            {
                auto found = stream::findStory(*client, chronicle, story, false, readyUntil);
                if(found.ok())
                {
                    ready = true;
                    break;
                }
                if(!absl::IsNotFound(found.status()))
                {
                    std::cerr << found.status() << '\n';
                    return 1;
                }
                pause(std::chrono::milliseconds(100));
            }
            if(!ready)
            {
                std::cerr << "metric story not ready: " << story << '\n';
                return stopping ? 0 : 1;
            }
        }
        stream::Exporter exporter(*client, positions, chronicle, stories, sink, exportOptions);
        auto result = exporter.run([] { return stopping != 0; }, deadline);
        if(!result.ok())
        {
            std::cerr << result.status() << '\n';
            return 1;
        }
        std::cout << "exported=" << result->events << " batches=" << result->batches << '\n';
#endif
        return 0;
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
