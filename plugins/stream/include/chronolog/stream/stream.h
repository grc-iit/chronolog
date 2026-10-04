#pragma once
#include "chronolog/kvs/store.h"
#include <array>
#include <functional>
#include <nlohmann/json.hpp>

namespace chronolog::stream
{
using Json = nlohmann::json;
struct Metric
{
    std::string name, unit;
    Json values;
    std::map<std::string, std::string> labels;
};
using CpuCounters = std::array<uint64_t, 8>;
absl::StatusOr<CpuCounters> parseCpu(const std::string& path);
absl::StatusOr<Metric> parseMemory(const std::string& path);
absl::StatusOr<std::vector<Metric>> parseNetwork(const std::string& path, bool loopback = false);
Metric cpuUtilization(const CpuCounters& before, const CpuCounters& after);
absl::StatusOr<std::string> lineProtocol(const Event&);
struct ProcPaths
{
    std::string cpu{"/proc/stat"}, memory{"/proc/meminfo"}, network{"/proc/net/dev"};
};
class Collector
{
public:
    Collector(client::Client&, std::string chronicle, std::string host, ProcPaths = {}, bool loopback = false);
    absl::StatusOr<client::BatchResult>
    sample(Durability = Durability::Durable, Envelope metadata = {}, client::Deadline = {});

private:
    client::Client& client_;
    std::string chronicle_, host_, identity_;
    ProcPaths paths_;
    bool loopback_;
    std::optional<CpuCounters> previous_;
};
absl::StatusOr<StoryId>
findStory(client::Client&, const std::string& chronicle, const std::string& name, bool create, client::Deadline = {});
struct SinkOptions
{
    std::string url{"http://127.0.0.1:8086"}, org{"chronolog"}, bucket{"telemetry"}, token{};
    size_t retries{3};
    std::chrono::milliseconds timeout{2000}, backoff{50}, max_backoff{1000};
};
class InfluxSink
{
public:
    explicit InfluxSink(SinkOptions options)
        : options_(std::move(options))
    {}
    absl::Status post(const std::string&, client::Deadline = {});

private:
    SinkOptions options_;
};
class Batch
{
public:
    Batch(size_t max_count, std::chrono::milliseconds max_age, size_t max_bytes = 1u << 20);
    bool full(size_t additional) const;
    bool due(std::chrono::steady_clock::time_point now) const;
    absl::Status add(const std::string& story, client::Position, std::string line);
    const std::string& body() const { return body_; }
    const std::map<std::string, client::Position>& positions() const { return positions_; }
    size_t count() const { return count_; }
    void clear();

private:
    size_t max_count_, max_bytes_, count_{};
    std::chrono::milliseconds max_age_;
    std::chrono::steady_clock::time_point started_;
    std::string body_;
    std::map<std::string, client::Position> positions_;
};
struct ExportOptions
{
    std::string consumer{"influx"};
    size_t batch_count{128};
    std::chrono::milliseconds batch_age{250}, pull_timeout{25};
};
struct ExportStats
{
    size_t events{}, batches{};
};
class Exporter
{
public:
    Exporter(client::Client&,
             kvs::Store&,
             std::string chronicle,
             std::vector<std::string> stories,
             InfluxSink&,
             ExportOptions = {});
    absl::StatusOr<ExportStats> run(std::function<bool()> stop, client::Deadline = {});
    absl::StatusOr<std::optional<client::Position>> saved(const std::string&, client::Deadline = {});

private:
    absl::Status flush(Batch&, ExportStats&, client::Deadline);
    client::Client& client_;
    kvs::Store& positions_;
    std::string chronicle_;
    std::vector<std::string> stories_;
    InfluxSink& sink_;
    ExportOptions options_;
};
} // namespace chronolog::stream
