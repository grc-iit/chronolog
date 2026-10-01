#include "chronolog/stream/stream.h"
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>

namespace chronolog::stream
{
namespace
{
client::Deadline bounded(client::Deadline deadline)
{
    return deadline.value_or(std::chrono::system_clock::now() + std::chrono::seconds(10));
}
} // namespace
absl::StatusOr<CpuCounters> parseCpu(const std::string& path)
{
    std::ifstream file(path);
    if(!file)
        return absl::NotFoundError("cannot open " + path);
    std::string line;
    std::getline(file, line);
    if(line.size() > 4096)
        return absl::InvalidArgumentError("oversized /proc/stat line");
    std::istringstream fields(line);
    std::string label;
    CpuCounters counters{};
    fields >> label;
    if(label != "cpu")
        return absl::InvalidArgumentError("missing aggregate cpu counters");
    for(auto& value: counters)
    {
        std::string token;
        if(!(fields >> token) || token.empty() || token.front() == '-')
            return absl::InvalidArgumentError("invalid cpu counter");
        try
        {
            size_t used{};
            value = std::stoull(token, &used);
            if(used != token.size())
                return absl::InvalidArgumentError("invalid cpu counter");
        }
        catch(...)
        {
            return absl::InvalidArgumentError("invalid cpu counter");
        }
    }
    return counters;
}
Metric cpuUtilization(const CpuCounters& before, const CpuCounters& after)
{
    static const std::array<std::string, 8> modes =
            {"user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal"};
    std::array<double, 8> delta{};
    double total{};
    for(size_t i = 0; i < delta.size(); ++i)
    {
        delta[i] = after[i] >= before[i] ? static_cast<double>(after[i] - before[i]) : 0;
        total += delta[i];
    }
    Json values = Json::object();
    for(size_t i = 0; i < delta.size(); ++i) values[modes[i]] = total ? delta[i] / total : 0;
    values["total"] = total ? 1 - (delta[3] + delta[4]) / total : 0;
    return {"system.cpu.utilization", "1", values, {}};
}
absl::StatusOr<Metric> parseMemory(const std::string& path)
{
    std::ifstream file(path);
    if(!file)
        return absl::NotFoundError("cannot open " + path);
    std::optional<uint64_t> total, available;
    std::string line;
    for(size_t n = 0; n < 512 && std::getline(file, line); ++n)
    {
        if(line.size() > 4096)
            return absl::InvalidArgumentError("oversized meminfo line");
        std::istringstream fields(line);
        std::string key, value, unit;
        fields >> key;
        if(key != "MemTotal:" && key != "MemAvailable:")
            continue;
        if(!(fields >> value >> unit) || unit != "kB" || value.empty() || value.front() == '-')
            return absl::InvalidArgumentError("invalid memory counter");
        try
        {
            size_t used{};
            auto kb = std::stoull(value, &used);
            if(used != value.size() || kb > UINT64_MAX / 1024)
                return absl::InvalidArgumentError("memory counter overflow");
            if(key == "MemTotal:")
                total = kb * 1024;
            else
                available = kb * 1024;
        }
        catch(...)
        {
            return absl::InvalidArgumentError("invalid memory counter");
        }
    }
    if(!total || !available || *available > *total)
        return absl::InvalidArgumentError("invalid total/available memory");
    return Metric{"system.memory.usage",
                  "By",
                  {{"total", *total}, {"available", *available}, {"used", *total - *available}},
                  {}};
}
absl::StatusOr<std::vector<Metric>> parseNetwork(const std::string& path, bool loopback)
{
    std::ifstream file(path);
    if(!file)
        return absl::NotFoundError("cannot open " + path);
    std::string line;
    std::getline(file, line);
    std::getline(file, line);
    std::vector<Metric> metrics;
    while(std::getline(file, line))
    {
        if(line.size() > 4096 || metrics.size() >= 256)
            return absl::ResourceExhaustedError("network interface limit");
        auto colon = line.find(':');
        if(colon == std::string::npos)
            return absl::InvalidArgumentError("invalid network interface line");
        std::istringstream nameField(line.substr(0, colon));
        std::string name;
        nameField >> name;
        if(name.empty())
            return absl::InvalidArgumentError("missing network interface name");
        if(name == "lo" && !loopback)
            continue;
        std::istringstream fields(line.substr(colon + 1));
        std::array<uint64_t, 16> counters{};
        for(auto& counter: counters)
        {
            std::string value;
            if(!(fields >> value) || value.empty() || value.front() == '-')
                return absl::InvalidArgumentError("invalid network counter");
            try
            {
                size_t used{};
                counter = std::stoull(value, &used);
                if(used != value.size())
                    return absl::InvalidArgumentError("invalid network counter");
            }
            catch(...)
            {
                return absl::InvalidArgumentError("invalid network counter");
            }
        }
        metrics.push_back({"system.network.io",
                           "By",
                           {{"rx_bytes", counters[0]},
                            {"rx_packets", counters[1]},
                            {"tx_bytes", counters[8]},
                            {"tx_packets", counters[9]}},
                           {{"network.interface.name", name}}});
    }
    return metrics;
}
absl::StatusOr<StoryId> findStory(client::Client& client,
                                  const std::string& chronicle,
                                  const std::string& name,
                                  bool create,
                                  client::Deadline deadline)
{
    deadline = bounded(deadline);
    if(create)
    {
        auto c = client.createChronicle(chronicle, deadline);
        if(!c.ok() && !absl::IsAlreadyExists(c.status()))
            return c.status();
        auto s = client.createStory(chronicle, name, deadline);
        if(s.ok())
            return s->id;
        if(!absl::IsAlreadyExists(s.status()))
            return s.status();
    }
    auto stories = client.listStories(chronicle, deadline);
    if(!stories.ok())
        return stories.status();
    for(const auto& s: *stories)
        if(s.name == name && !s.tombstoned)
            return s.id;
    return absl::NotFoundError("metric story " + name + " not found");
}
Collector::Collector(client::Client& client, std::string chronicle, std::string host, ProcPaths paths, bool loopback)
    : client_(client)
    , chronicle_(std::move(chronicle))
    , host_(std::move(host))
    , paths_(std::move(paths))
    , loopback_(loopback)
{
    static std::atomic<uint64_t> serial{};
    identity_ = "stream-" + host_ + "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(++serial);
}
absl::StatusOr<client::BatchResult>
Collector::sample(Durability durability, Envelope metadata, client::Deadline deadline)
{
    deadline = bounded(deadline);
    auto cpu = parseCpu(paths_.cpu);
    if(!cpu.ok())
        return cpu.status();
    auto memory = parseMemory(paths_.memory);
    if(!memory.ok())
        return memory.status();
    auto network = parseNetwork(paths_.network, loopback_);
    if(!network.ok())
        return network.status();
    std::vector<Metric> metrics = {cpuUtilization(previous_.value_or(*cpu), *cpu), *memory};
    previous_ = *cpu;
    metrics.insert(metrics.end(), network->begin(), network->end());
    client::BatchResult receipts;
    for(const auto& family: {"system.cpu", "system.memory", "system.network"})
    {
        auto id = findStory(client_, chronicle_, family, true, deadline);
        if(!id.ok())
            return id.status();
        std::vector<client::AppendSpec> batch;
        for(const auto& metric: metrics)
        {
            if(metric.name.rfind(family, 0) != 0)
                continue;
            client::AppendSpec spec;
            spec.durability = durability;
            spec.envelope = metadata;
            spec.envelope.content_type = "application/vnd.chronolog.metric+json";
            auto labels = metric.labels;
            labels["host.name"] = host_;
            spec.envelope.payload =
                    Json({{"name", metric.name}, {"unit", metric.unit}, {"values", metric.values}, {"labels", labels}})
                            .dump();
            spec.envelope.attributes["host.name"] = host_;
            spec.envelope.attributes["unit"] = metric.unit;
            spec.envelope.attributes["metric.name"] = metric.name;
            for(const auto& [key, value]: metric.labels) spec.envelope.attributes[key] = value;
            batch.push_back(std::move(spec));
        }
        if(batch.empty())
            continue;
        auto writer = client_.acquire(*id, identity_, deadline);
        if(!writer.ok())
            return writer.status();
        auto appended = writer->appendBatch(batch, deadline);
        auto released = writer->release(deadline);
        (void)released;
        if(!appended.ok())
            return appended.status();
        for(auto& receipt: *appended) receipts.push_back(std::move(receipt));
    }
    return receipts;
}
} // namespace chronolog::stream
