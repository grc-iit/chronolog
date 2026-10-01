#include "chronolog_ldms_bridge.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include "chronolog/client/client.h"
#include "core.h"

namespace sdk = chronolog::client;
namespace ldms = chronolog::ldms;
using Json = nlohmann::json;
using namespace std::chrono_literals;

namespace
{
constexpr size_t kMaxSample = 1u << 20;
constexpr auto kLogInterval = 5s;
std::atomic<uint64_t> bridgeSerial{0};

void copyError(char* dst, size_t cap, const std::string& text)
{
    if(dst == nullptr || cap == 0)
        return;
    std::snprintf(dst, cap, "%s", text.c_str());
}
void stderrLog(void*, const char* message) { std::fprintf(stderr, "chronolog-ldms: %s\n", message); }
int64_t systemNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}
} // namespace

struct Sample
{
    chrono_ldms_story* stream;
    uint64_t timestamp_ns;
    std::string json;
};

struct chrono_ldms_story
{
    chrono_ldms* bridge{};
    std::string chronicle, story, schema, producer;
    std::optional<chronolog::StoryId> id;
    std::optional<sdk::Writer> writer;
};

struct chrono_ldms
{
    chrono_ldms(sdk::Client c, const chrono_ldms_config& cfg, size_t queueCapacity, size_t batchSize)
        : client(std::move(c))
        , capacity(queueCapacity)
        , batch(batchSize)
        , flushInterval(std::chrono::milliseconds(cfg.flush_interval_ms ? cfg.flush_interval_ms : 100))
        , rpcTimeout(std::chrono::milliseconds(cfg.rpc_timeout_ms ? cfg.rpc_timeout_ms : 10000))
        , durability(cfg.durability == CHRONO_LDMS_ACCEPTED ? chronolog::Durability::Accepted
                                                            : chronolog::Durability::Durable)
        , log(cfg.log ? cfg.log : stderrLog)
        , logCtx(cfg.log ? cfg.log_ctx : nullptr)
        , ring(queueCapacity)
    {
        char host[128] = "unknown";
        ::gethostname(host, sizeof(host) - 1);
        identity =
                std::string("ldms-") + host + "-" + std::to_string(::getpid()) + "-" + std::to_string(++bridgeSerial);
    }

    sdk::Deadline deadline() const { return std::chrono::system_clock::now() + rpcTimeout; }
    void setError(const std::string& text)
    {
        std::lock_guard lock(errorMutex);
        lastError = text;
    }
    void wakeDrainer()
    {
        if(sleeping.load())
            wake.notify_one();
    }
    // Flush and close wake the drainer under its mutex so the wait predicate cannot miss them.
    void wakeForControl()
    {
        {
            std::lock_guard lock(wakeMutex);
        }
        wake.notify_all();
    }
    void markFinished(size_t n)
    {
        finished.fetch_add(n);
        {
            std::lock_guard lock(doneMutex);
        }
        done.notify_all();
    }
    void failAll(std::vector<std::unique_ptr<Sample>>& group, const std::string& why)
    {
        setError(why);
        failed.fetch_add(group.size());
        queued.fetch_sub(group.size());
        markFinished(group.size());
    }
    absl::StatusOr<chronolog::StoryId> resolve(const chrono_ldms_story& st);
    void send(chrono_ldms_story& st, std::vector<std::unique_ptr<Sample>>& group);
    void drain();
    void maybeLog(bool force);

    sdk::Client client;
    const size_t capacity, batch;
    const std::chrono::milliseconds flushInterval, rpcTimeout;
    const chronolog::Durability durability;
    const chrono_ldms_log_fn log;
    void* const logCtx;
    std::string identity;
    ldms::BoundedQueue<Sample*> ring;
    std::atomic<uint64_t> queued{0}, enqueued{0}, finished{0}, appended{0}, dropped{0}, failed{0};
    std::atomic<bool> closing{false}, abandon{false}, sleeping{false};
    std::atomic<int> flushers{0};
    std::mutex streamsMutex, errorMutex, wakeMutex, doneMutex;
    std::condition_variable wake, done;
    std::map<std::string, std::unique_ptr<chrono_ldms_story>> streams;
    std::string lastError;
    uint64_t loggedDropped{0}, loggedFailed{0};
    std::chrono::steady_clock::time_point lastLog{};
    std::thread drainer;
};

absl::StatusOr<chronolog::StoryId> chrono_ldms::resolve(const chrono_ldms_story& st)
{
    auto chronicle = client.createChronicle(st.chronicle, deadline());
    if(!chronicle.ok() && !absl::IsAlreadyExists(chronicle.status()))
        return chronicle.status();
    auto story = client.createStory(st.chronicle, st.story, deadline());
    if(story.ok())
        return story->id;
    if(!absl::IsAlreadyExists(story.status()))
        return story.status();
    auto stories = client.listStories(st.chronicle, deadline());
    if(!stories.ok())
        return stories.status();
    for(const auto& s: *stories)
        if(s.name == st.story && !s.tombstoned)
            return s.id;
    return absl::NotFoundError("story " + st.story + " not found after ALREADY_EXISTS");
}

void chrono_ldms::send(chrono_ldms_story& st, std::vector<std::unique_ptr<Sample>>& group)
{
    if(!st.id)
    {
        auto id = resolve(st);
        if(!id.ok())
            return failAll(group, "resolve " + st.chronicle + "/" + st.story + ": " + id.status().ToString());
        st.id = *id;
    }
    if(!st.writer)
    {
        auto writer = client.acquire(*st.id, identity + "/" + st.story, deadline());
        if(!writer.ok())
        {
            st.id.reset();
            return failAll(group, "acquire " + st.story + ": " + writer.status().ToString());
        }
        st.writer.emplace(std::move(*writer));
    }
    const int64_t now = systemNowNs();
    std::vector<sdk::AppendSpec> specs;
    size_t invalid = 0;
    for(const auto& sample: group)
    {
        Json doc = Json::parse(sample->json, nullptr, false);
        if(!doc.is_object())
        {
            ++invalid;
            continue;
        }
        sdk::AppendSpec spec;
        spec.durability = durability;
        spec.envelope.content_type = "application/vnd.chronolog.ldms-sample+json";
        spec.envelope.payload = sample->json;
        spec.envelope.attributes["ldms.producer"] = st.producer;
        spec.envelope.attributes["ldms.schema"] = st.schema;
        auto instance = doc.find("instance");
        if(instance != doc.end() && instance->is_string())
            spec.envelope.attributes["ldms.instance"] = instance->get<std::string>();
        const bool inside = sample->timestamp_ns <= static_cast<uint64_t>(INT64_MAX) &&
                            ldms::inKeeperWindow(static_cast<int64_t>(sample->timestamp_ns), now);
        if(inside)
            spec.physical = chronolog::TimeReading{static_cast<int64_t>(sample->timestamp_ns),
                                                   std::nullopt,
                                                   chronolog::ClockStatus::Unsynced};
        else
            spec.envelope.attributes["ldms.timestamp_ns"] = std::to_string(sample->timestamp_ns);
        specs.push_back(std::move(spec));
    }
    if(invalid != 0)
    {
        setError("sample is not a JSON object");
        failed.fetch_add(invalid);
    }
    size_t okCount = 0;
    if(!specs.empty())
    {
        auto result = st.writer->appendBatch(specs, deadline());
        if(!result.ok())
        {
            st.writer.reset();
            st.id.reset();
            setError("append " + st.story + ": " + result.status().ToString());
            failed.fetch_add(specs.size());
        }
        else
            for(const auto& item: *result)
            {
                const bool ok = item.ok() && (durability != chronolog::Durability::Durable || item->acked());
                if(ok)
                    ++okCount;
                else
                {
                    failed.fetch_add(1);
                    setError("append " + st.story + ": " +
                             (item.ok() ? std::string("not acked") : item.status().ToString()));
                }
            }
    }
    appended.fetch_add(okCount);
    queued.fetch_sub(group.size());
    markFinished(group.size());
}

void chrono_ldms::maybeLog(bool force)
{
    const auto now = std::chrono::steady_clock::now();
    const uint64_t d = dropped.load(), f = failed.load();
    if((d == loggedDropped && f == loggedFailed) || (!force && now - lastLog < kLogInterval))
        return;
    std::string error;
    {
        std::lock_guard lock(errorMutex);
        error = lastError;
    }
    char text[512];
    std::snprintf(text,
                  sizeof(text),
                  "dropped %llu (+%llu) failed %llu (+%llu) last error: %s",
                  static_cast<unsigned long long>(d),
                  static_cast<unsigned long long>(d - loggedDropped),
                  static_cast<unsigned long long>(f),
                  static_cast<unsigned long long>(f - loggedFailed),
                  error.empty() ? "none" : error.c_str());
    log(logCtx, text);
    loggedDropped = d;
    loggedFailed = f;
    lastLog = now;
}

void chrono_ldms::drain()
{
    std::vector<std::unique_ptr<Sample>> pending;
    auto oldest = std::chrono::steady_clock::now();
    for(;;)
    {
        const bool stopping = closing.load();
        Sample* sample = nullptr;
        while(pending.size() < batch && ring.pop(sample))
        {
            if(pending.empty())
                oldest = std::chrono::steady_clock::now();
            pending.emplace_back(sample);
        }
        if(abandon.load() && !pending.empty())
        {
            failAll(pending, "abandoned at close");
            pending.clear();
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool full = pending.size() >= batch;
        const bool aged = !pending.empty() && now - oldest >= flushInterval;
        const bool force = flushers.load() > 0 || stopping;
        if(!pending.empty() && (full || aged || force))
        {
            std::map<chrono_ldms_story*, std::vector<std::unique_ptr<Sample>>> groups;
            for(auto& s: pending)
            {
                auto* key = s->stream;
                groups[key].push_back(std::move(s));
            }
            pending.clear();
            for(auto& [stream, group]: groups) send(*stream, group);
            maybeLog(false);
            continue;
        }
        if(pending.empty() && stopping && ring.pop(sample))
        {
            pending.emplace_back(sample);
            continue;
        }
        if(pending.empty() && stopping)
            break;
        maybeLog(false);
        auto wait = pending.empty()
                            ? std::min<std::chrono::milliseconds>(flushInterval, 20ms)
                            : std::max<std::chrono::milliseconds>(1ms,
                                                                  std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                          flushInterval - (now - oldest)));
        std::unique_lock lock(wakeMutex);
        sleeping.store(true);
        if(ring.pop(sample))
        {
            sleeping.store(false);
            if(pending.empty())
                oldest = std::chrono::steady_clock::now();
            pending.emplace_back(sample);
            continue;
        }
        wake.wait_for(lock, wait, [&] { return flushers.load() > 0 || closing.load(); });
        sleeping.store(false);
    }
    maybeLog(true);
}

extern "C"
{
    int chrono_ldms_open(const chrono_ldms_config* config, chrono_ldms_t** out, char* error, size_t error_cap)
    {
        if(out == nullptr)
            return CHRONO_LDMS_INVALID_ARGUMENT;
        *out = nullptr;
        if(config == nullptr || config->catalog_endpoint == nullptr || config->catalog_endpoint[0] == '\0')
        {
            copyError(error, error_cap, "catalog_endpoint is required");
            return CHRONO_LDMS_INVALID_ARGUMENT;
        }
        const size_t capacity = config->queue_capacity ? config->queue_capacity : 8192;
        const size_t batch =
                std::min<size_t>(config->batch_size ? config->batch_size : 128, std::min<size_t>(capacity, 10000));
        sdk::ClientOptions options;
        options.catalog_endpoint = config->catalog_endpoint;
        if(config->player_endpoint != nullptr)
            options.player_endpoint = config->player_endpoint;
        options.rpc_timeout = std::chrono::milliseconds(config->rpc_timeout_ms ? config->rpc_timeout_ms : 10000);
        auto client = sdk::Client::Connect(options, std::chrono::system_clock::now() + options.rpc_timeout);
        if(!client.ok())
        {
            copyError(error, error_cap, "connect: " + client.status().ToString());
            return CHRONO_LDMS_UNAVAILABLE;
        }
        auto* bridge = new(std::nothrow) chrono_ldms(std::move(*client), *config, capacity, batch);
        if(bridge == nullptr)
        {
            copyError(error, error_cap, "out of memory");
            return CHRONO_LDMS_UNAVAILABLE;
        }
        bridge->drainer = std::thread([bridge] { bridge->drain(); });
        *out = bridge;
        return CHRONO_LDMS_OK;
    }

    int chrono_ldms_stream(chrono_ldms_t* bridge,
                           const char* container,
                           const char* schema,
                           const char* producer,
                           chrono_ldms_stream_t** out)
    {
        if(bridge == nullptr || out == nullptr || container == nullptr || schema == nullptr || producer == nullptr ||
           container[0] == '\0' || schema[0] == '\0' || producer[0] == '\0')
            return CHRONO_LDMS_INVALID_ARGUMENT;
        if(bridge->closing.load())
            return CHRONO_LDMS_CLOSED;
        const std::string chronicle = ldms::sanitizeName(container);
        const std::string story = ldms::sanitizeName(schema) + "_" + ldms::sanitizeName(producer);
        const std::string key = chronicle + '\n' + story;
        std::lock_guard lock(bridge->streamsMutex);
        auto& slot = bridge->streams[key];
        if(!slot)
        {
            slot = std::make_unique<chrono_ldms_story>();
            slot->bridge = bridge;
            slot->chronicle = chronicle;
            slot->story = story;
            slot->schema = schema;
            slot->producer = producer;
        }
        *out = slot.get();
        return CHRONO_LDMS_OK;
    }

    int chrono_ldms_store(chrono_ldms_stream_t* stream, uint64_t timestamp_ns, const char* json, size_t len)
    {
        if(stream == nullptr || json == nullptr || len == 0 || len > kMaxSample)
            return CHRONO_LDMS_INVALID_ARGUMENT;
        chrono_ldms* b = stream->bridge;
        if(b->closing.load(std::memory_order_acquire))
            return CHRONO_LDMS_CLOSED;
        if(b->queued.fetch_add(1) >= b->capacity)
        {
            b->queued.fetch_sub(1);
            b->dropped.fetch_add(1);
            return CHRONO_LDMS_DROPPED;
        }
        auto* sample = new(std::nothrow) Sample{stream, timestamp_ns, std::string()};
        bool pushed = false;
        if(sample != nullptr)
        {
            sample->json.assign(json, len);
            b->enqueued.fetch_add(1);
            pushed = b->ring.push(sample);
            if(!pushed)
                b->enqueued.fetch_sub(1);
        }
        if(!pushed)
        {
            delete sample;
            b->queued.fetch_sub(1);
            b->dropped.fetch_add(1);
            return CHRONO_LDMS_DROPPED;
        }
        b->wakeDrainer();
        return CHRONO_LDMS_OK;
    }

    int64_t chrono_ldms_encode(const chrono_ldms_sample* sample, char* out, size_t cap)
    {
        if(sample == nullptr || sample->producer == nullptr || sample->schema == nullptr ||
           (sample->metric_count != 0 && sample->metrics == nullptr))
            return -1;
        Json metrics = Json::object();
        for(size_t i = 0; i < sample->metric_count; ++i)
        {
            const auto& m = sample->metrics[i];
            if(m.name == nullptr)
                return -1;
            switch(m.kind)
            {
                case CHRONO_LDMS_I64:
                    metrics[m.name] = m.v.i64;
                    break;
                case CHRONO_LDMS_U64:
                    metrics[m.name] = m.v.u64;
                    break;
                case CHRONO_LDMS_F64:
                    metrics[m.name] = m.v.f64;
                    break;
                default:
                    return -1;
            }
        }
        const std::string text = Json({{"producer", sample->producer},
                                       {"instance", sample->instance ? sample->instance : ""},
                                       {"schema", sample->schema},
                                       {"component_id", sample->component_id},
                                       {"timestamp_ns", sample->timestamp_ns},
                                       {"metrics", std::move(metrics)}})
                                         .dump();
        if(out != nullptr && cap > 0)
        {
            const size_t n = std::min(text.size(), cap - 1);
            std::memcpy(out, text.data(), n);
            out[n] = '\0';
        }
        return static_cast<int64_t>(text.size());
    }

    int chrono_ldms_store_sample(chrono_ldms_t* bridge, const char* container, const chrono_ldms_sample* sample)
    {
        if(sample == nullptr)
            return CHRONO_LDMS_INVALID_ARGUMENT;
        chrono_ldms_stream_t* stream = nullptr;
        const int rc = chrono_ldms_stream(bridge, container, sample->schema, sample->producer, &stream);
        if(rc != CHRONO_LDMS_OK)
            return rc;
        char stack[2048];
        const int64_t len = chrono_ldms_encode(sample, stack, sizeof(stack));
        if(len < 0)
            return CHRONO_LDMS_INVALID_ARGUMENT;
        if(static_cast<size_t>(len) < sizeof(stack))
            return chrono_ldms_store(stream, sample->timestamp_ns, stack, static_cast<size_t>(len));
        std::string heap(static_cast<size_t>(len) + 1, '\0');
        chrono_ldms_encode(sample, heap.data(), heap.size());
        return chrono_ldms_store(stream, sample->timestamp_ns, heap.data(), static_cast<size_t>(len));
    }

    int chrono_ldms_flush(chrono_ldms_t* bridge, uint32_t timeout_ms)
    {
        if(bridge == nullptr)
            return CHRONO_LDMS_INVALID_ARGUMENT;
        const uint64_t target = bridge->enqueued.load();
        bridge->flushers.fetch_add(1);
        bridge->wakeForControl();
        bool ok;
        {
            std::unique_lock lock(bridge->doneMutex);
            ok = bridge->done.wait_for(lock,
                                       std::chrono::milliseconds(timeout_ms),
                                       [&] { return bridge->finished.load() >= target; });
        }
        bridge->flushers.fetch_sub(1);
        return ok ? CHRONO_LDMS_OK : CHRONO_LDMS_TIMEOUT;
    }

    void chrono_ldms_stats_get(chrono_ldms_t* bridge, chrono_ldms_stats* out)
    {
        if(bridge == nullptr || out == nullptr)
            return;
        out->queued = bridge->queued.load();
        out->appended = bridge->appended.load();
        out->dropped = bridge->dropped.load();
        out->failed = bridge->failed.load();
        std::lock_guard lock(bridge->errorMutex);
        copyError(out->last_error, sizeof(out->last_error), bridge->lastError);
    }

    int chrono_ldms_close(chrono_ldms_t* bridge, uint32_t drain_timeout_ms)
    {
        if(bridge == nullptr)
            return CHRONO_LDMS_INVALID_ARGUMENT;
        bridge->closing.store(true, std::memory_order_release);
        bridge->wakeForControl();
        const uint64_t target = bridge->enqueued.load();
        bool drained;
        {
            std::unique_lock lock(bridge->doneMutex);
            drained = bridge->done.wait_for(lock,
                                            std::chrono::milliseconds(drain_timeout_ms),
                                            [&] { return bridge->finished.load() >= target; });
        }
        if(!drained)
            bridge->abandon.store(true);
        bridge->wakeForControl();
        bridge->drainer.join();
        for(auto& [key, stream]: bridge->streams)
            if(stream->writer)
                (void)stream->writer->release(std::chrono::system_clock::now() + 2s);
        Sample* leftover = nullptr;
        while(bridge->ring.pop(leftover)) delete leftover;
        delete bridge;
        return drained ? CHRONO_LDMS_OK : CHRONO_LDMS_TIMEOUT;
    }
}
