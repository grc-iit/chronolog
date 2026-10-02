// Closed loop load generator over the client SDK. One JSON line per measured cell on stdout and in --out.
// Scenarios: append, replay, tail, archive. run.sh starts the stack and adds the metadata record.
// The loops are closed: a worker issues the next request when the previous one returns. An open loop mode
// would replace issueNext() pacing in appendWorker() and tailWriter() with a schedule, nothing else.
#include "chronolog/client/client.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <span>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace sdk = chronolog::client;
using chronolog::Hlc;
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

namespace
{
std::ofstream outFile;

[[noreturn]] void die(const std::string& message)
{
    std::cerr << "bench_load: " << message << '\n';
    std::exit(2);
}

class Args
{
public:
    Args(int argc, char** argv)
    {
        for(int i = 2; i < argc; ++i)
        {
            const std::string key = argv[i];
            if(key.rfind("--", 0) != 0 || i + 1 >= argc)
                die("expected --key value pairs, got " + key);
            values_[key.substr(2)] = argv[++i];
        }
    }
    std::string get(const std::string& key, const std::string& fallback = {}) const
    {
        const auto found = values_.find(key);
        return found == values_.end() ? fallback : found->second;
    }
    int64_t num(const std::string& key, int64_t fallback) const
    {
        const auto found = values_.find(key);
        return found == values_.end() ? fallback : std::stoll(found->second);
    }
    double real(const std::string& key, double fallback) const
    {
        const auto found = values_.find(key);
        return found == values_.end() ? fallback : std::stod(found->second);
    }
    Json asJson() const
    {
        Json json = Json::object();
        for(const auto& [key, value]: values_)
            if(key != "out" && key != "catalog" && key != "player" && key != "keeper-log" && key != "archive-root")
                json[key] = value;
        return json;
    }

private:
    std::map<std::string, std::string> values_;
};

void emit(const std::string& suite, const Json& params, const Json& result)
{
    Json record = {{"layer", "service"}, {"suite", suite}, {"params", params}, {"result", result}};
    const auto line = record.dump();
    std::cout << line << std::endl;
    if(outFile.is_open())
        outFile << line << std::endl;
}

std::unique_ptr<sdk::Client> connect(const Args& args)
{
    sdk::ClientOptions options;
    options.catalog_endpoint = args.get("catalog");
    options.player_endpoint = args.get("player");
    options.rpc_timeout = std::chrono::seconds(30);
    auto client = sdk::Client::Connect(options);
    if(!client.ok())
        die("connect: " + client.status().ToString());
    return std::make_unique<sdk::Client>(std::move(*client));
}

double micros(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }
double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

double percentile(const std::vector<double>& sorted, double p)
{
    if(sorted.empty())
        return 0;
    const auto index = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

Json latencyJson(std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    return {{"samples", samples.size()},
            {"p50_us", percentile(samples, 0.50)},
            {"p99_us", percentile(samples, 0.99)},
            {"p999_us", percentile(samples, 0.999)},
            {"max_us", samples.empty() ? 0.0 : samples.back()}};
}

struct Opened
{
    std::unique_ptr<sdk::Client> client;
    chronolog::StoryId story{};
    std::unique_ptr<sdk::Writer> writer;
};

Opened openStory(const Args& args,
                 sdk::Client& admin,
                 const std::string& chronicle,
                 const std::string& story,
                 const std::string& identity)
{
    auto created = admin.createStory(chronicle, story);
    if(!created.ok())
        die("createStory: " + created.status().ToString());
    Opened opened;
    opened.client = connect(args);
    opened.story = created->id;
    auto writer = opened.client->acquire(created->id, identity);
    if(!writer.ok())
        die("acquire: " + writer.status().ToString());
    opened.writer = std::make_unique<sdk::Writer>(std::move(*writer));
    return opened;
}

// ---------------------------------------------------------------------------------------------------------------
// append
// ---------------------------------------------------------------------------------------------------------------
struct AppendStats
{
    std::vector<double> latencies_us;
    uint64_t events{}, errors{};
    Clock::time_point last{};
    std::string first_error;
};

void appendWorker(sdk::Writer& writer,
                  size_t payload,
                  size_t batch,
                  bool durable,
                  Clock::time_point warm_end,
                  Clock::time_point end,
                  uint64_t cap_bytes,
                  std::atomic<uint64_t>& written,
                  AppendStats& stats)
{
    std::vector<sdk::AppendSpec> specs(batch);
    for(auto& spec: specs)
    {
        spec.envelope.payload.assign(payload, 'x');
        spec.durability = durable ? chronolog::Durability::Durable : chronolog::Durability::Accepted;
    }
    for(;;)
    {
        const auto t0 = Clock::now();
        if(t0 >= end || written.load(std::memory_order_relaxed) >= cap_bytes)
            break;
        uint64_t ok = 0, failed = 0;
        std::string error;
        if(batch == 1)
        {
            auto result = writer.append(specs[0]);
            if(result.ok())
                ++ok;
            else
            {
                ++failed;
                error = result.status().ToString();
            }
        }
        else
        {
            auto result = writer.appendBatch(specs);
            if(!result.ok())
            {
                failed = batch;
                error = result.status().ToString();
            }
            else
                for(const auto& item: *result)
                {
                    if(item.ok())
                        ++ok;
                    else
                    {
                        ++failed;
                        error = item.status().ToString();
                    }
                }
        }
        const auto t1 = Clock::now();
        written.fetch_add(ok * payload, std::memory_order_relaxed);
        if(t0 >= warm_end)
        {
            stats.latencies_us.push_back(micros(t1 - t0));
            stats.events += ok;
            stats.errors += failed;
            stats.last = t1;
            if(failed != 0 && stats.first_error.empty())
                stats.first_error = error;
        }
    }
}

int appendScenario(const Args& args)
{
    const auto payload = static_cast<size_t>(args.num("payload", 1024));
    const auto batch = static_cast<size_t>(args.num("batch", 1));
    const auto writers = static_cast<size_t>(args.num("writers", 1));
    const bool durable = args.get("durability", "durable") != "accepted";
    const double run_seconds = args.real("seconds", 5), warmup = args.real("warmup", 1);
    const uint64_t cap_bytes = static_cast<uint64_t>(args.num("max-mb", 1024)) << 20;
    const std::string name = "bench-append-" + args.get("cell", std::to_string(::getpid()));

    auto admin = connect(args);
    if(auto created = admin->createChronicle(name); !created.ok())
        die("createChronicle: " + created.status().ToString());
    std::vector<Opened> opened;
    for(size_t i = 0; i < writers; ++i)
        opened.push_back(openStory(args, *admin, name, "w" + std::to_string(i), "bench"));

    std::vector<AppendStats> stats(writers);
    std::atomic<uint64_t> written{0};
    const auto start = Clock::now();
    const auto warm_end = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(warmup));
    const auto end = warm_end + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(run_seconds));
    std::vector<std::thread> threads;
    for(size_t i = 0; i < writers; ++i)
        threads.emplace_back(appendWorker,
                             std::ref(*opened[i].writer),
                             payload,
                             batch,
                             durable,
                             warm_end,
                             end,
                             cap_bytes,
                             std::ref(written),
                             std::ref(stats[i]));
    for(auto& thread: threads) thread.join();

    std::vector<double> latencies;
    uint64_t events = 0, errors = 0;
    Clock::time_point last = warm_end;
    std::string first_error;
    for(const auto& s: stats)
    {
        latencies.insert(latencies.end(), s.latencies_us.begin(), s.latencies_us.end());
        events += s.events;
        errors += s.errors;
        last = std::max(last, s.last);
        if(first_error.empty())
            first_error = s.first_error;
    }
    const double elapsed = std::max(seconds(last - warm_end), 1e-9);
    Json result = latencyJson(latencies);
    result["calls"] = latencies.size();
    result["events"] = events;
    result["errors"] = errors;
    result["first_error"] = first_error;
    result["measured_seconds"] = elapsed;
    result["events_per_s"] = static_cast<double>(events) / elapsed;
    result["payload_mb_per_s"] = static_cast<double>(events * payload) / elapsed / 1e6;
    result["capped_by_max_mb"] = written.load() >= cap_bytes;
    for(auto& o: opened) (void)o.writer->release();
    emit("append", args.asJson(), result);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// settled chunks, read from the Keeper log the same way tests/smoke/python/smoke.py does
// ---------------------------------------------------------------------------------------------------------------
using Bound = std::pair<int64_t, uint32_t>;

std::vector<std::pair<Bound, Bound>> settledChunks(const std::string& log, chronolog::StoryId story)
{
    std::vector<std::pair<Bound, Bound>> chunks;
    std::ifstream in(log);
    std::string line;
    while(std::getline(in, line))
    {
        const auto at = line.find("archive_settled");
        if(at == std::string::npos)
            continue;
        unsigned long long id = 0;
        long long s = 0, e = 0;
        unsigned sl = 0, el = 0;
        if(std::sscanf(line.c_str() + at,
                       "archive_settled chunk=%*s story=%llu start=%lld:%u end=%lld:%u",
                       &id,
                       &s,
                       &sl,
                       &e,
                       &el) != 5 ||
           id != story)
            continue;
        chunks.push_back({{s, sl}, {e, el}});
    }
    std::sort(chunks.begin(), chunks.end());
    return chunks;
}

// Seconds until settled chunks cover [first, last] without a gap, or a negative number after the bound.
double waitSettled(const std::string& log,
                   chronolog::StoryId story,
                   Hlc first,
                   Hlc last,
                   double bound_seconds,
                   Clock::time_point since,
                   size_t& chunk_count)
{
    const Bound a{first.physical_ns, first.logical}, b{last.physical_ns, last.logical};
    for(;;)
    {
        bool started = false;
        Bound reach{};
        size_t used = 0;
        for(const auto& [start, end]: settledChunks(log, story))
        {
            if(!started)
            {
                if(start <= a)
                {
                    started = true;
                    reach = end;
                    ++used;
                }
            }
            else if(start <= reach)
            {
                reach = std::max(reach, end);
                ++used;
            }
        }
        if(started && reach > b)
        {
            chunk_count = used;
            return seconds(Clock::now() - since);
        }
        if(seconds(Clock::now() - since) > bound_seconds)
            return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct Written
{
    chronolog::StoryId story{};
    std::vector<Hlc> hlcs;
    Clock::time_point first_append, last_ack;
};

Written writeEvents(const Args& args,
                    sdk::Client& admin,
                    const std::string& chronicle,
                    const std::string& story,
                    size_t events,
                    size_t payload)
{
    auto opened = openStory(args, admin, chronicle, story, "bench");
    Written written;
    written.story = opened.story;
    std::vector<sdk::AppendSpec> specs(256);
    for(auto& spec: specs) spec.envelope.payload.assign(payload, 'x');
    written.first_append = Clock::now();
    while(written.hlcs.size() < events)
    {
        const auto count = std::min<size_t>(256, events - written.hlcs.size());
        auto result = opened.writer->appendBatch(std::span<const sdk::AppendSpec>(specs.data(), count));
        if(!result.ok())
            die("appendBatch: " + result.status().ToString());
        for(const auto& item: *result)
        {
            if(!item.ok())
                die("append item: " + item.status().ToString());
            written.hlcs.push_back(item->hlc);
        }
    }
    written.last_ack = Clock::now();
    (void)opened.writer->release();
    return written;
}

// ---------------------------------------------------------------------------------------------------------------
// replay
// ---------------------------------------------------------------------------------------------------------------
struct ReadOutcome
{
    uint64_t events{}, bytes{};
    double total_s{}, first_s{};
    bool complete{};
    int attempts{};
    std::string error;
};

ReadOutcome readRange(sdk::Client& client, chronolog::StoryId story, Hlc start, Hlc end)
{
    ReadOutcome best;
    for(int attempt = 1; attempt <= 30; ++attempt)
    {
        ReadOutcome out;
        out.attempts = attempt;
        const auto t0 = Clock::now();
        auto stream = client.read(story, {start, end});
        if(!stream.ok())
        {
            out.error = stream.status().ToString();
            best = out;
            break;
        }
        bool got_first = false;
        for(;;)
        {
            auto item = stream->next(std::chrono::system_clock::now() + std::chrono::seconds(60));
            if(!item.ok())
            {
                out.error = item.status().ToString();
                break;
            }
            if(!*item)
                break;
            if(!got_first && !(**item).events.empty())
            {
                got_first = true;
                out.first_s = seconds(Clock::now() - t0);
            }
            for(const auto& event: (**item).events)
            {
                ++out.events;
                out.bytes += event.envelope.payload.size();
            }
            if((**item).completion)
            {
                out.complete = (**item).completion->complete;
                break;
            }
        }
        out.total_s = seconds(Clock::now() - t0);
        best = out;
        if(out.complete || !out.error.empty())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return best;
}

Json readJson(const std::string& range, const ReadOutcome& out)
{
    return {{"range", range},
            {"events", out.events},
            {"complete", out.complete},
            {"attempts", out.attempts},
            {"error", out.error},
            {"seconds", out.total_s},
            {"first_event_seconds", out.first_s},
            {"events_per_s", out.total_s > 0 ? static_cast<double>(out.events) / out.total_s : 0.0},
            {"payload_mb_per_s", out.total_s > 0 ? static_cast<double>(out.bytes) / out.total_s / 1e6 : 0.0}};
}

int replayScenario(const Args& args)
{
    const auto events = static_cast<size_t>(args.num("events", 100000));
    const auto payload = static_cast<size_t>(args.num("payload", 1024));
    const std::string profile = args.get("profile", "hot");
    const int runs = static_cast<int>(args.num("runs", 3));
    const std::string name = "bench-replay-" + std::to_string(::getpid());
    auto admin = connect(args);
    if(auto created = admin->createChronicle(name); !created.ok())
        die("createChronicle: " + created.status().ToString());
    const auto written = writeEvents(args, *admin, name, "s", events, payload);
    auto end_of = [](Hlc h)
    {
        ++h.logical;
        return h;
    };
    Json cells = Json::array();
    double settle_s = 0;
    if(profile == "cold")
    {
        size_t chunks = 0;
        settle_s = waitSettled(args.get("keeper-log"),
                               written.story,
                               written.hlcs.front(),
                               written.hlcs.back(),
                               args.real("settle-bound", 120),
                               written.last_ack,
                               chunks);
        if(settle_s < 0)
            die("archive did not settle within the bound");
    }
    auto reader = connect(args);
    std::vector<std::pair<std::string, std::pair<Hlc, Hlc>>> ranges;
    if(profile == "cold")
        ranges.push_back({"cold_first_half", {written.hlcs.front(), written.hlcs[events / 2]}});
    ranges.push_back(
            {profile == "cold" ? "mixed_full" : "hot_full", {written.hlcs.front(), end_of(written.hlcs.back())}});
    for(const auto& [label, range]: ranges)
        for(int run = 0; run < runs; ++run)
        {
            auto cell = readJson(label, readRange(*reader, written.story, range.first, range.second));
            cell["run"] = run;
            cells.push_back(cell);
        }
    emit("replay",
         args.asJson(),
         {{"profile", profile}, {"events_written", events}, {"settle_after_last_ack_s", settle_s}, {"reads", cells}});
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// tail: append-to-receipt latency in one process, so the timestamps share a clock
// ---------------------------------------------------------------------------------------------------------------
int64_t steadyNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

int tailScenario(const Args& args)
{
    const auto events = static_cast<size_t>(args.num("events", 2000));
    const auto payload = std::max<size_t>(8, static_cast<size_t>(args.num("payload", 1024)));
    const double rate = args.real("rate", 200);
    constexpr size_t warm = 20;
    const std::string name = "bench-tail-" + std::to_string(::getpid());
    auto admin = connect(args);
    if(auto created = admin->createChronicle(name); !created.ok())
        die("createChronicle: " + created.status().ToString());
    auto opened = openStory(args, *admin, name, "s", "bench");
    auto reader = connect(args);
    auto tail = reader->tail(opened.story);
    if(!tail.ok())
        die("tail: " + tail.status().ToString());

    std::vector<double> latencies;
    std::atomic<bool> done{false};
    std::atomic<size_t> received{0};
    std::string tail_error;
    std::atomic<bool> tail_failed{false};
    std::string consumer_exit = "done";
    Clock::time_point last_receipt = Clock::now();
    const auto tail_started = Clock::now();
    std::thread consumer(
            [&]
            {
                while(!done.load())
                {
                    auto item = tail->next(std::chrono::system_clock::now() + std::chrono::seconds(1));
                    if(!item.ok())
                    {
                        if(item.status().code() == absl::StatusCode::kDeadlineExceeded)
                            continue;
                        if(done.load() && item.status().code() == absl::StatusCode::kCancelled)
                            return;
                        tail_error = item.status().ToString();
                        tail_failed = true;
                        return;
                    }
                    if(!*item)
                    {
                        consumer_exit = "end_of_stream";
                        return;
                    }
                    const auto now = steadyNs();
                    last_receipt = Clock::now();
                    for(const auto& event: (**item).events)
                    {
                        int64_t sent = 0;
                        std::memcpy(&sent, event.envelope.payload.data(), sizeof(sent));
                        const auto index = received.fetch_add(1);
                        if(index >= warm)
                            latencies.push_back(static_cast<double>(now - sent) / 1000.0);
                    }
                }
            });
    std::vector<double> ack_us;
    std::string payload_bytes(payload, 'x');
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
    auto next = Clock::now();
    for(size_t i = 0; i < events + warm; ++i)
    {
        std::this_thread::sleep_until(next);
        next += period;
        sdk::AppendSpec spec;
        const auto sent = steadyNs();
        std::memcpy(payload_bytes.data(), &sent, sizeof(sent));
        spec.envelope.payload = payload_bytes;
        const auto t0 = Clock::now();
        auto result = opened.writer->append(spec);
        if(!result.ok())
            die("append: " + result.status().ToString());
        if(i >= warm)
            ack_us.push_back(micros(Clock::now() - t0));
    }
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while(received.load() < events + warm && Clock::now() < deadline && !tail_failed.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    done = true;
    tail->cancel();
    consumer.join();
    (void)opened.writer->release();
    Json result = {{"receipt", latencyJson(latencies)},
                   {"append_ack", latencyJson(ack_us)},
                   {"received", received.load() > warm ? received.load() - warm : 0},
                   {"expected", events},
                   {"tail_error", tail_error},
                   {"consumer_exit", consumer_exit},
                   {"last_receipt_after_tail_start_s", seconds(last_receipt - tail_started)}};
    emit("tail", args.asJson(), result);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// archive: how long after the last acknowledged append the Keeper settles every chunk with the Grapher
// ---------------------------------------------------------------------------------------------------------------
uint64_t directoryBytes(const std::string& root)
{
    uint64_t total = 0;
    std::error_code ec;
    for(std::filesystem::recursive_directory_iterator it(root, ec), stop; !ec && it != stop; it.increment(ec))
        if(it->is_regular_file(ec))
            total += it->file_size(ec);
    return total;
}

int archiveScenario(const Args& args)
{
    const auto events = static_cast<size_t>(args.num("events", 100000));
    const auto payload = static_cast<size_t>(args.num("payload", 1024));
    const std::string name = "bench-archive-" + std::to_string(::getpid());
    auto admin = connect(args);
    if(auto created = admin->createChronicle(name); !created.ok())
        die("createChronicle: " + created.status().ToString());
    const auto written = writeEvents(args, *admin, name, "s", events, payload);
    size_t chunks = 0;
    const double settle = waitSettled(args.get("keeper-log"),
                                      written.story,
                                      written.hlcs.front(),
                                      written.hlcs.back(),
                                      args.real("settle-bound", 120),
                                      written.last_ack,
                                      chunks);
    const double write_s = seconds(written.last_ack - written.first_append);
    Json result = {{"events", events},
                   {"write_seconds", write_s},
                   {"settle_after_last_ack_s", settle},
                   {"settled_chunks_used", chunks},
                   {"archive_bytes", directoryBytes(args.get("archive-root"))}};
    if(settle >= 0)
        result["archive_mb_per_s_end_to_end"] =
                static_cast<double>(result["archive_bytes"].get<uint64_t>()) / (write_s + settle) / 1e6;
    emit("archive", args.asJson(), result);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc < 2)
        die("usage: chronolog_bench_load append|replay|tail|archive --catalog HOST:PORT --player HOST:PORT [--key "
            "value ...]");
    const std::string scenario = argv[1];
    const Args args(argc, argv);
    if(!args.get("out").empty())
        outFile.open(args.get("out"), std::ios::app);
    if(scenario == "append")
        return appendScenario(args);
    if(scenario == "replay")
        return replayScenario(args);
    if(scenario == "tail")
        return tailScenario(args);
    if(scenario == "archive")
        return archiveScenario(args);
    die("unknown scenario " + scenario);
}
