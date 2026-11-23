#include <ClientConfiguration.h>
#include <chronolog_client.h>
#if CHRONOLOG_HAS_MONOTONIC_TIMER
#include "ChronologTimer.h"
#include "ChronologClientImpl.h"
#else
#include "StorytellerClient.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace chronolog;
using Clock = std::chrono::steady_clock;
using Sample = uint64_t;
struct Options
{
    unsigned threads = 1;
    unsigned iterations = 1000;
    unsigned warmup = 200;
    unsigned keepers = 1;
    bool diagnose_cleanup = false;
    size_t payload_bytes = 128;
    std::string mode = "normal";
    std::string api = "timer";
    std::string config;
    std::string chronicle;
    std::string story = "append-bench";
};
unsigned number(char const* value, char const* name)
{
    char* end = nullptr;
    auto parsed = std::strtoul(value, &end, 10);
    if(!value[0] || *end || parsed == 0)
        throw std::runtime_error(std::string("invalid ") + name);
    return static_cast<unsigned>(parsed);
}
Options parse(int argc, char** argv)
{
    Options options;
    for(int i = 1; i < argc; ++i)
    {
        std::string key = argv[i];
        if(key == "--help")
        {
            std::cout << "--api timer|log_event|append_event --mode normal|frozen|rollback "
                         "--threads 1|2|4|8 --iterations N --warmup N --payload N "
                         "--config PATH --chronicle NAME --story NAME --keepers N\n";
            std::exit(0);
        }
        if(i + 1 == argc)
            throw std::runtime_error("missing value for " + key);
        auto const* value = argv[++i];
        if(key == "--threads")
            options.threads = number(value, "threads");
        else if(key == "--iterations")
            options.iterations = number(value, "iterations");
        else if(key == "--warmup")
            options.warmup = number(value, "warmup");
        else if(key == "--payload")
            options.payload_bytes = number(value, "payload");
        else if(key == "--diagnose-cleanup")
            options.diagnose_cleanup = number(value, "diagnose-cleanup") != 0;
        else if(key == "--keepers")
            options.keepers = number(value, "keepers");
        else if(key == "--mode")
            options.mode = value;
        else if(key == "--api")
            options.api = value;
        else if(key == "--config")
            options.config = value;
        else if(key == "--chronicle")
            options.chronicle = value;
        else if(key == "--story")
            options.story = value;
        else
            throw std::runtime_error("unknown option: " + key);
    }
    if(options.threads != 1 && options.threads != 2 && options.threads != 4 && options.threads != 8)
        throw std::runtime_error("threads must be one of 1, 2, 4, 8");
    if(options.mode != "normal" && options.mode != "frozen" && options.mode != "rollback")
        throw std::runtime_error("mode must be normal, frozen, or rollback");
    if(options.api != "timer" && options.api != "log_event" && options.api != "append_event")
        throw std::runtime_error("api must be timer, log_event or append_event");
    if(options.api == "timer" && options.mode != "normal" && !CHRONOLOG_HAS_MONOTONIC_TIMER)
        throw std::runtime_error("injected clock modes are available only with ChronologTimer");
    if(options.api != "timer" && options.mode != "normal")
        throw std::runtime_error("public Client does not expose an injected clock; use --api timer");
    if(options.api != "timer" && options.config.empty())
        throw std::runtime_error("--config is required for Client append measurements");
    if(options.api != "timer" && options.chronicle.empty())
        throw std::runtime_error("--chronicle is required for Client append measurements");
#if !CHRONOLOG_HAS_MONOTONIC_TIMER
    if(options.api == "append_event")
        throw std::runtime_error("append_event is unavailable in this source version");
#endif
    return options;
}
uint64_t percentile(std::vector<Sample> const& samples, double fraction)
{
    if(samples.empty())
        return 0;
    auto index = static_cast<size_t>(fraction * static_cast<double>(samples.size() - 1));
    return samples[index];
}
struct ClockFixture
{
    std::atomic<bool> measuring{false};
    std::atomic<uint64_t> measured_calls{0};
    std::atomic<uint64_t> rollback_raw_calls{0};
    uint64_t raw() noexcept
    {
        if(!measuring.load(std::memory_order_acquire))
            return 1700000000000000000ULL;
        auto call = measured_calls.fetch_add(1, std::memory_order_relaxed);
        if(call == 0)
            return 1700000000000000100ULL;
        rollback_raw_calls.fetch_add(1, std::memory_order_relaxed);
        return 1699999999000000000ULL;
    }
};
#if CHRONOLOG_HAS_MONOTONIC_TIMER
chronolog::ChronologTimer::RawSource sourceFor(std::string const& mode, ClockFixture& fixture)
{
    if(mode == "normal")
        return {};
    if(mode == "frozen")
        return [] { return chronolog::TimestampResult{CL_SUCCESS, 1700000000000000000ULL}; };
    return [&fixture] { return chronolog::TimestampResult{CL_SUCCESS, fixture.raw()}; };
}
void runTimer(Options const& options)
{
    ClockFixture fixture;
    chronolog::ChronologTimer timer(sourceFor(options.mode, fixture));
    auto execute = [&](unsigned count,
                       std::vector<Sample>* samples,
                       std::vector<uint64_t>* timestamps,
                       std::atomic<uint64_t>& successes,
                       std::atomic<uint64_t>& failures)
    {
        for(unsigned i = 0; i < count; ++i)
        {
            auto const started = Clock::now();
            auto result = timer.next(started + std::chrono::seconds(5));
            auto const elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
            if(samples)
                samples->push_back(static_cast<Sample>(elapsed));
            if(result.status == CL_SUCCESS)
            {
                ++successes;
                if(timestamps)
                    timestamps->push_back(result.timestamp);
            }
            else
                ++failures;
        }
    };
    std::atomic<uint64_t> warmup_success{0}, warmup_failure{0};
    execute(options.warmup, nullptr, nullptr, warmup_success, warmup_failure);
    fixture.measured_calls = 0;
    fixture.measuring.store(true, std::memory_order_release);
    std::vector<std::vector<Sample>> per_thread(options.threads);
    std::vector<std::vector<uint64_t>> per_thread_timestamps(options.threads);
    std::vector<std::thread> threads;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::atomic<uint64_t> successes{0}, failures{0};
    threads.reserve(options.threads);
    for(unsigned i = 0; i < options.threads; ++i)
        threads.emplace_back(
                [&, i]
                {
                    ready.fetch_add(1, std::memory_order_release);
                    while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
                    per_thread[i].reserve(options.iterations);
                    per_thread_timestamps[i].reserve(options.iterations);
                    execute(options.iterations, &per_thread[i], &per_thread_timestamps[i], successes, failures);
                });
    while(ready.load(std::memory_order_acquire) != options.threads) std::this_thread::yield();
    auto const measured_started = Clock::now();
    start.store(true, std::memory_order_release);
    for(auto& thread: threads) thread.join();
    auto const measured_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - measured_started).count();
    std::vector<Sample> samples;
    std::vector<uint64_t> timestamps;
    for(auto const& values: per_thread) samples.insert(samples.end(), values.begin(), values.end());
    for(auto const& values: per_thread_timestamps) timestamps.insert(timestamps.end(), values.begin(), values.end());
    std::sort(samples.begin(), samples.end());
    std::sort(timestamps.begin(), timestamps.end());
    uint64_t duplicates = 0;
    for(size_t i = 1; i < timestamps.size(); ++i)
        if(timestamps[i] == timestamps[i - 1])
            ++duplicates;
    std::cout << std::fixed << std::setprecision(2) << "{\"api\":\"timer\",\"mode\":\"" << options.mode
              << "\",\"threads\":" << options.threads << ",\"warmup_total\":" << options.warmup
              << ",\"iterations_per_thread\":" << options.iterations << ",\"samples\":" << samples.size()
              << ",\"successes\":" << successes.load() << ",\"failures\":" << failures.load()
              << ",\"warmup_successes\":" << warmup_success.load() << ",\"warmup_failures\":" << warmup_failure.load()
              << ",\"p50_ns\":" << percentile(samples, .50) << ",\"p95_ns\":" << percentile(samples, .95)
              << ",\"p99_ns\":" << percentile(samples, .99)
              << ",\"throughput_ops_s\":" << (1e9 * samples.size() / measured_ns)
              << ",\"measured_wall_ns\":" << measured_ns << ",\"timestamp_count\":" << timestamps.size()
              << ",\"timestamp_min\":" << (timestamps.empty() ? 0 : timestamps.front())
              << ",\"timestamp_max\":" << (timestamps.empty() ? 0 : timestamps.back())
              << ",\"timestamp_duplicates\":" << duplicates
              << ",\"injected_raw_calls\":" << fixture.measured_calls.load()
              << ",\"rollback_raw_calls\":" << fixture.rollback_raw_calls.load() << "}\n";
}
#else
void runTimer(Options const& options)
{
    chronolog::ChronologTimer timer;
    auto execute = [&](unsigned count,
                       std::vector<Sample>* samples,
                       std::vector<uint64_t>* timestamps,
                       std::atomic<uint64_t>& successes,
                       std::atomic<uint64_t>& failures)
    {
        for(unsigned i = 0; i < count; ++i)
        {
            auto const started = Clock::now();
            auto const timestamp = timer.getTimestamp();
            auto const elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
            if(samples)
                samples->push_back(static_cast<Sample>(elapsed));
            if(timestamp != 0)
            {
                ++successes;
                if(timestamps)
                    timestamps->push_back(timestamp);
            }
            else
                ++failures;
        }
    };
    std::atomic<uint64_t> warmup_success{0}, warmup_failure{0};
    execute(options.warmup, nullptr, nullptr, warmup_success, warmup_failure);
    std::vector<std::vector<Sample>> per_thread(options.threads);
    std::vector<std::vector<uint64_t>> per_thread_timestamps(options.threads);
    std::vector<std::thread> threads;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::atomic<uint64_t> successes{0}, failures{0};
    threads.reserve(options.threads);
    for(unsigned i = 0; i < options.threads; ++i)
        threads.emplace_back(
                [&, i]
                {
                    ready.fetch_add(1, std::memory_order_release);
                    while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
                    per_thread[i].reserve(options.iterations);
                    per_thread_timestamps[i].reserve(options.iterations);
                    execute(options.iterations, &per_thread[i], &per_thread_timestamps[i], successes, failures);
                });
    while(ready.load(std::memory_order_acquire) != options.threads) std::this_thread::yield();
    auto const measured_started = Clock::now();
    start.store(true, std::memory_order_release);
    for(auto& thread: threads) thread.join();
    auto const measured_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - measured_started).count();
    std::vector<Sample> samples;
    std::vector<uint64_t> timestamps;
    for(auto const& values: per_thread) samples.insert(samples.end(), values.begin(), values.end());
    for(auto const& values: per_thread_timestamps) timestamps.insert(timestamps.end(), values.begin(), values.end());
    std::sort(samples.begin(), samples.end());
    std::sort(timestamps.begin(), timestamps.end());
    uint64_t duplicates = 0;
    for(size_t i = 1; i < timestamps.size(); ++i)
        if(timestamps[i] == timestamps[i - 1])
            ++duplicates;
    std::cout << std::fixed << std::setprecision(2)
              << "{\"api\":\"timer\",\"mode\":\"normal\",\"timer_source\":\"baseline-high-resolution-clock-ticks\","
                 "\"threads\":"
              << options.threads << ",\"warmup_total\":" << options.warmup
              << ",\"iterations_per_thread\":" << options.iterations << ",\"samples\":" << samples.size()
              << ",\"successes\":" << successes.load() << ",\"failures\":" << failures.load()
              << ",\"warmup_successes\":" << warmup_success.load() << ",\"warmup_failures\":" << warmup_failure.load()
              << ",\"p50_ns\":" << percentile(samples, .50) << ",\"p95_ns\":" << percentile(samples, .95)
              << ",\"p99_ns\":" << percentile(samples, .99)
              << ",\"throughput_ops_s\":" << (1e9 * samples.size() / measured_ns)
              << ",\"measured_wall_ns\":" << measured_ns << ",\"timestamp_count\":" << timestamps.size()
              << ",\"timestamp_min_ticks\":" << (timestamps.empty() ? 0 : timestamps.front())
              << ",\"timestamp_max_ticks\":" << (timestamps.empty() ? 0 : timestamps.back())
              << ",\"timestamp_duplicates\":" << duplicates << "}\n";
}
#endif
void runClient(Options const& options)
{
    chronolog::ClientConfiguration configuration;
    if(!configuration.load_from_file(options.config))
        throw std::runtime_error("cannot load client config: " + options.config);
    chronolog::ClientPortalServiceConf portal;
    portal.PROTO_CONF = configuration.PORTAL_CONF.PROTO_CONF;
    portal.IP = configuration.PORTAL_CONF.IP;
    portal.PORT = configuration.PORTAL_CONF.PORT;
    portal.PROVIDER_ID = configuration.PORTAL_CONF.PROVIDER_ID;
    auto client_owner = std::make_unique<chronolog::Client>(portal);
    auto& client = *client_owner;
    if(client.Connect() != CL_SUCCESS)
        throw std::runtime_error("Client::Connect failed");
    auto create_status = client.CreateChronicle(options.chronicle);
    if(create_status != CL_SUCCESS)
        throw std::runtime_error("Client::CreateChronicle failed: " + std::to_string(create_status));
    auto acquired = client.AcquireStory(options.chronicle, options.story);
    if(acquired.first != CL_SUCCESS || acquired.second == nullptr)
        throw std::runtime_error("Client::AcquireStory failed: " + std::to_string(acquired.first));
    std::string const payload(options.payload_bytes, 'x');
    struct OutcomeCounts
    {
        std::atomic<uint64_t> success{0}, failure{0}, rejected{0}, not_submitted{0}, unconfirmed{0};
        std::atomic<uint64_t> status_unavailable{0};
        std::mutex status_mutex;
        std::map<int, uint64_t> append_statuses;
    };
    OutcomeCounts warmup_counts;
    auto execute = [&](unsigned count,
                       std::vector<Sample>* samples,
                       std::vector<Sample>* successful_samples,
                       std::vector<uint64_t>* timestamps,
                       OutcomeCounts& counts)
    {
        for(unsigned i = 0; i < count; ++i)
        {
            auto const started = Clock::now();
            uint64_t timestamp = 0;
#if CHRONOLOG_HAS_MONOTONIC_TIMER
            if(options.api == "append_event")
            {
                auto result = acquired.second->append_event(payload);
                {
                    std::lock_guard<std::mutex> lock(counts.status_mutex);
                    ++counts.append_statuses[result.status];
                }
                if(result.outcome == chronolog::AppendOutcome::Acknowledged && result.status == CL_SUCCESS)
                    timestamp = result.timestamp;
                else if(result.outcome == chronolog::AppendOutcome::Rejected)
                {
                    ++counts.failure;
                    ++counts.rejected;
                }
                else if(result.outcome == chronolog::AppendOutcome::Unconfirmed)
                {
                    ++counts.failure;
                    ++counts.unconfirmed;
                }
                else
                {
                    ++counts.failure;
                    ++counts.not_submitted;
                }
            }
            else
#endif
            {
                timestamp = acquired.second->log_event(payload);
                if(timestamp == 0)
                {
                    ++counts.failure;
                    ++counts.status_unavailable;
                }
            }
            if(timestamp != 0)
            {
                ++counts.success;
                if(timestamps)
                    timestamps->push_back(timestamp);
            }
            auto const elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count();
            if(samples)
                samples->push_back(static_cast<Sample>(elapsed));
            if(timestamp != 0 && successful_samples)
                successful_samples->push_back(static_cast<Sample>(elapsed));
        }
    };
    execute(options.warmup, nullptr, nullptr, nullptr, warmup_counts);
    std::vector<std::vector<Sample>> per_thread(options.threads);
    std::vector<std::vector<Sample>> per_thread_successful(options.threads);
    std::vector<std::vector<uint64_t>> per_thread_timestamps(options.threads);
    std::vector<std::thread> threads;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    OutcomeCounts measured_counts;
    threads.reserve(options.threads);
    for(unsigned i = 0; i < options.threads; ++i)
        threads.emplace_back(
                [&, i]
                {
                    ready.fetch_add(1, std::memory_order_release);
                    while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
                    per_thread[i].reserve(options.iterations);
                    per_thread_successful[i].reserve(options.iterations);
                    per_thread_timestamps[i].reserve(options.iterations);
                    execute(options.iterations,
                            &per_thread[i],
                            &per_thread_successful[i],
                            &per_thread_timestamps[i],
                            measured_counts);
                });
    while(ready.load(std::memory_order_acquire) != options.threads) std::this_thread::yield();
    auto const measured_started = Clock::now();
    start.store(true, std::memory_order_release);
    for(auto& thread: threads) thread.join();
    auto const measured_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - measured_started).count();
    std::vector<Sample> samples;
    std::vector<Sample> successful_samples;
    std::vector<uint64_t> timestamps;
    for(auto const& values: per_thread) samples.insert(samples.end(), values.begin(), values.end());
    for(auto const& values: per_thread_successful)
        successful_samples.insert(successful_samples.end(), values.begin(), values.end());
    for(auto const& values: per_thread_timestamps) timestamps.insert(timestamps.end(), values.begin(), values.end());
    std::sort(samples.begin(), samples.end());
    std::sort(successful_samples.begin(), successful_samples.end());
    std::sort(timestamps.begin(), timestamps.end());
    uint64_t routes[16]{};
    if(options.keepers <= 16)
        for(auto timestamp: timestamps) ++routes[timestamp % options.keepers];
    std::cout << std::fixed << std::setprecision(2) << "{\"api\":\"" << options.api
              << "\",\"mode\":\"normal\",\"threads\":" << options.threads << ",\"transport\":\"" << portal.PROTO_CONF
              << "\",\"keepers\":" << options.keepers << ",\"payload_bytes\":" << options.payload_bytes
              << ",\"warmup_total\":" << options.warmup << ",\"iterations_per_thread\":" << options.iterations
              << ",\"samples\":" << samples.size() << ",\"successes\":" << measured_counts.success.load()
              << ",\"failures\":" << measured_counts.failure.load()
              << ",\"rejected\":" << measured_counts.rejected.load()
              << ",\"not_submitted\":" << measured_counts.not_submitted.load()
              << ",\"status_unavailable_failures\":" << measured_counts.status_unavailable.load()
              << ",\"unconfirmed\":" << measured_counts.unconfirmed.load()
              << ",\"warmup_successes\":" << warmup_counts.success.load()
              << ",\"warmup_failures\":" << warmup_counts.failure.load()
              << ",\"warmup_rejected\":" << warmup_counts.rejected.load()
              << ",\"warmup_not_submitted\":" << warmup_counts.not_submitted.load()
              << ",\"warmup_status_unavailable_failures\":" << warmup_counts.status_unavailable.load()
              << ",\"warmup_unconfirmed\":" << warmup_counts.unconfirmed.load()
              << ",\"p50_ns\":" << percentile(samples, .50) << ",\"p95_ns\":" << percentile(samples, .95)
              << ",\"p99_ns\":" << percentile(samples, .99)
              << ",\"success_p50_ns\":" << percentile(successful_samples, .50)
              << ",\"success_p95_ns\":" << percentile(successful_samples, .95)
              << ",\"success_p99_ns\":" << percentile(successful_samples, .99)
              << ",\"throughput_ops_s\":" << (1e9 * samples.size() / measured_ns)
              << ",\"measured_wall_ns\":" << measured_ns << ",\"timestamp_count\":" << timestamps.size()
              << ",\"timestamp_min\":" << (timestamps.empty() ? 0 : timestamps.front())
              << ",\"timestamp_max\":" << (timestamps.empty() ? 0 : timestamps.back())
              << ",\"predicted_route_counts\":[";
    if(options.keepers <= 16)
        for(unsigned i = 0; i < options.keepers; ++i) std::cout << (i ? "," : "") << routes[i];
    std::cout << "],\"append_status_counts\":[";
    bool first_status = true;
    for(auto const& [status, count]: measured_counts.append_statuses)
    {
        std::cout << (first_status ? "" : ",") << "{\"status\":" << status << ",\"count\":" << count << "}";
        first_status = false;
    }
    std::cout << "]}\n";
    auto cleanup_started = Clock::now();
    auto cleanup_expires = cleanup_started + std::chrono::seconds(5);
    auto release_status = client.ReleaseStory(options.chronicle, options.story);
    auto destroy_story_status = client.DestroyStory(options.chronicle, options.story);
    auto destroy_chronicle_status = client.DestroyChronicle(options.chronicle);
    auto disconnect_status = client.Disconnect();
    std::cerr << "{\"cleanup\":{\"release_story\":" << release_status << ",\"destroy_story\":" << destroy_story_status
              << ",\"destroy_chronicle\":" << destroy_chronicle_status << ",\"disconnect\":" << disconnect_status
              << "}}\n";
#if CHRONOLOG_HAS_MONOTONIC_TIMER
    if(options.diagnose_cleanup)
    {
        // Diagnostic observation starts after measurement. Retain the SAME
        // singleton owner; do not create a replacement to bypass admission.
        auto observer = chronolog::ChronologClientImpl::GetClientImplInstance(portal,
                                                                              chronolog::WRITER_MODE,
                                                                              chronolog::ClientQueryServiceConf{});
        auto context = observer->context();
        auto runtime = observer->runtime();
        bool same_client = observer->client_id() == client.client_id();
        std::cerr << "{\"diagnostic_same_client\":" << (same_client ? "true" : "false") << "}\n";
        auto settle_capacity = [&](char const* name, int status, auto operation)
        {
            unsigned attempts = 0;
            while(status == CL_ERR_RPC_CAPACITY && Clock::now() < cleanup_expires)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if(Clock::now() >= cleanup_expires)
                    break;
                status = operation();
                auto elapsed =
                        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - cleanup_started).count();
                std::cerr << "{\"cleanup_retry\":\"" << name << "\",\"attempt\":" << ++attempts
                          << ",\"status\":" << status << ",\"stage_elapsed_us\":" << elapsed << "}\n";
            }
            return status;
        };
        release_status = settle_capacity("release_story",
                                         release_status,
                                         [&] { return client.ReleaseStory(options.chronicle, options.story); });
        destroy_story_status = settle_capacity("destroy_story",
                                               destroy_story_status,
                                               [&] { return client.DestroyStory(options.chronicle, options.story); });
        destroy_chronicle_status = settle_capacity("destroy_chronicle",
                                                   destroy_chronicle_status,
                                                   [&] { return client.DestroyChronicle(options.chronicle); });
        disconnect_status = settle_capacity("disconnect", disconnect_status, [&] { return client.Disconnect(); });
        observer.reset();
        client_owner.reset();
        auto drain_status =
                runtime->drain(chronolog::rpc::RpcDeadline::fromStart(cleanup_started, std::chrono::seconds(5)));
        bool complete = context->cleanupComplete.load(std::memory_order_acquire);
        bool native_empty = complete && !context->engine && !context->procedures && !context->storyteller &&
                            !context->visor && !context->reader;
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - cleanup_started).count();
        std::cerr << "{\"cleanup_diagnostic\":{\"release_story\":" << release_status
                  << ",\"destroy_story\":" << destroy_story_status
                  << ",\"destroy_chronicle\":" << destroy_chronicle_status << ",\"disconnect\":" << disconnect_status
                  << ",\"drain\":" << drain_status << ",\"cleanup_complete\":" << (complete ? "true" : "false")
                  << ",\"native_fields_empty\":" << (native_empty ? "true" : "false")
                  << ",\"stage_elapsed_us\":" << elapsed << "}}\n";
        if(!same_client || drain_status != CL_SUCCESS || !native_empty)
            throw std::runtime_error("Client diagnostic owner did not settle");
    }
#endif
    if(release_status != CL_SUCCESS || destroy_story_status != CL_SUCCESS || destroy_chronicle_status != CL_SUCCESS ||
       disconnect_status != CL_SUCCESS)
        throw std::runtime_error("Client cleanup failed");
}
} // namespace
int main(int argc, char** argv)
{
    try
    {
        auto const options = parse(argc, argv);
        if(options.api == "timer")
            runTimer(options);
        else
            runClient(options);
    }
    catch(std::exception const& error)
    {
        std::cerr << "monotonic_timer_bench: " << error.what() << '\n';
        return 2;
    }
    return 0;
}
