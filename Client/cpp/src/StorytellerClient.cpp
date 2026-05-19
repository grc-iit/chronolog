#include <atomic>
#include <chrono>
#include <climits>
#include <algorithm>
#include <deque>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

#include <thallium.hpp>

#include <chronolog_types.h>
#include <chronolog_profile.h>

#include "KeeperRecordingClient.h"
#include "StorytellerClient.h"
#include "PlaybackQueryRpcClient.h"

namespace tl = thallium;

namespace chl = chronolog;

namespace
{
uint64_t clientStatsNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
}

class ClientAppendStats
{
public:
    static ClientAppendStats& instance()
    {
        static ClientAppendStats stats;
        return stats;
    }

    void recordBatch(uint64_t event_count,
                     uint64_t payload_bytes,
                     uint64_t event_build_ns,
                     uint64_t keeper_select_ns,
                     uint64_t rpc_submit_ns,
                     uint64_t future_count)
    {
        if(event_count == 0)
        {
            return;
        }
        uint64_t const previous = eventCount.fetch_add(event_count, std::memory_order_relaxed);
        batchCount.fetch_add(1, std::memory_order_relaxed);
        payloadBytes.fetch_add(payload_bytes, std::memory_order_relaxed);
        eventBuildNs.fetch_add(event_build_ns, std::memory_order_relaxed);
        keeperSelectNs.fetch_add(keeper_select_ns, std::memory_order_relaxed);
        rpcSubmitNs.fetch_add(rpc_submit_ns, std::memory_order_relaxed);
        futureCount.fetch_add(future_count, std::memory_order_relaxed);
        updateMax(rpcSubmitMaxNs, rpc_submit_ns);
        maybeLogInterval(previous + event_count);
    }

    void recordFutureWait(uint64_t wait_ns)
    {
        futureWaitCount.fetch_add(1, std::memory_order_relaxed);
        futureWaitNs.fetch_add(wait_ns, std::memory_order_relaxed);
        updateMax(futureWaitMaxNs, wait_ns);
    }

    void logSummary(char const* context) const
    {
        uint64_t const events = eventCount.load(std::memory_order_relaxed);
        uint64_t const batches = batchCount.load(std::memory_order_relaxed);
        uint64_t const waits = futureWaitCount.load(std::memory_order_relaxed);
        LOG_INFO("[ClientAppendStats] context={} pid={} batch_count={} event_count={} payload_bytes={} "
                 "event_build_avg_us={} keeper_select_avg_us={} rpc_submit_avg_us={} rpc_submit_max_us={} "
                 "future_count={} future_wait_count={} future_wait_avg_us={} future_wait_max_us={}",
                 context,
                 static_cast<unsigned long long>(getpid()),
                 batches,
                 events,
                 payloadBytes.load(std::memory_order_relaxed),
                 avgUs(eventBuildNs.load(std::memory_order_relaxed), events),
                 avgUs(keeperSelectNs.load(std::memory_order_relaxed), events),
                 avgUs(rpcSubmitNs.load(std::memory_order_relaxed), batches),
                 nsToUs(rpcSubmitMaxNs.load(std::memory_order_relaxed)),
                 futureCount.load(std::memory_order_relaxed),
                 waits,
                 avgUs(futureWaitNs.load(std::memory_order_relaxed), waits),
                 nsToUs(futureWaitMaxNs.load(std::memory_order_relaxed)));
    }

private:
    ClientAppendStats() = default;

    static double avgUs(uint64_t total_ns, uint64_t count)
    {
        if(count == 0)
        {
            return 0.0;
        }
        return static_cast<double>(total_ns) / static_cast<double>(count) / 1000.0;
    }

    static double nsToUs(uint64_t ns) { return static_cast<double>(ns) / 1000.0; }

    static void updateMax(std::atomic<uint64_t>& target, uint64_t value)
    {
        uint64_t observed = target.load(std::memory_order_relaxed);
        while(observed < value && !target.compare_exchange_weak(observed, value, std::memory_order_relaxed))
        {}
    }

    static uint64_t statsIntervalEvents()
    {
        static uint64_t interval = []() {
            char const* value = std::getenv("CHRONOLOG_CLIENT_APPEND_STATS_INTERVAL_EVENTS");
            if(value == nullptr || *value == '\0')
            {
                return uint64_t{0};
            }
            char* end = nullptr;
            unsigned long long parsed = std::strtoull(value, &end, 10);
            if(end == value)
            {
                return uint64_t{0};
            }
            return static_cast<uint64_t>(parsed);
        }();
        return interval;
    }

    void maybeLogInterval(uint64_t current_event_count) const
    {
        uint64_t const interval = statsIntervalEvents();
        if(interval > 0 && current_event_count / interval > lastLoggedInterval.load(std::memory_order_relaxed))
        {
            lastLoggedInterval.store(current_event_count / interval, std::memory_order_relaxed);
            logSummary("client_append_interval");
        }
    }

    std::atomic<uint64_t> batchCount{0};
    std::atomic<uint64_t> eventCount{0};
    std::atomic<uint64_t> payloadBytes{0};
    std::atomic<uint64_t> eventBuildNs{0};
    std::atomic<uint64_t> keeperSelectNs{0};
    std::atomic<uint64_t> rpcSubmitNs{0};
    std::atomic<uint64_t> rpcSubmitMaxNs{0};
    std::atomic<uint64_t> futureCount{0};
    std::atomic<uint64_t> futureWaitCount{0};
    std::atomic<uint64_t> futureWaitNs{0};
    std::atomic<uint64_t> futureWaitMaxNs{0};
    mutable std::atomic<uint64_t> lastLoggedInterval{0};
};

class CompletedLogEventFutureState: public chronolog::LogEventFuture::State
{
public:
    explicit CompletedLogEventFutureState(uint64_t event_time) : eventTime(event_time) {}

    uint64_t wait() override { return eventTime; }

private:
    uint64_t eventTime;
};

class CompositeLogEventFutureState: public chronolog::LogEventFuture::State
{
public:
    explicit CompositeLogEventFutureState(std::vector<chronolog::LogEventFuture> futures)
        : futures(std::move(futures))
    {}

    uint64_t wait() override
    {
        uint64_t const wait_start_ns = clientStatsNowNs();
        uint64_t max_event_time = 0;
        for(auto& future: futures)
        {
            uint64_t const event_time = future.wait();
            if(event_time == 0)
            {
                ClientAppendStats::instance().recordFutureWait(clientStatsNowNs() - wait_start_ns);
                return 0;
            }
            max_event_time = std::max(max_event_time, event_time);
        }
        ClientAppendStats::instance().recordFutureWait(clientStatsNowNs() - wait_start_ns);
        return max_event_time;
    }

    std::size_t future_count() const override
    {
        std::size_t count = 0;
        for(auto const& future: futures)
        {
            count += future.future_count();
        }
        return count;
    }

private:
    std::vector<chronolog::LogEventFuture> futures;
};

uint64_t keeperTailCursorOverlapNs()
{
    char const* value = std::getenv("CHRONOLOG_KEEPER_TAIL_CURSOR_OVERLAP_NS");
    if(value == nullptr || *value == '\0')
    {
        return 0;
    }

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if(end == value)
    {
        return 0;
    }
    return static_cast<uint64_t>(parsed);
}

bool clientReplayStatsEnabled()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_REPLAY_STATS");
    if(value == nullptr || *value == '\0')
    {
        return false;
    }

    std::string const parsed(value);
    return parsed != "0" && parsed != "false" && parsed != "FALSE" && parsed != "off" && parsed != "OFF";
}

bool clientParallelTailRpcEnabled()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_PARALLEL_TAIL_RPC");
    if(value == nullptr || *value == '\0')
    {
        return true;
    }

    std::string const parsed(value);
    return parsed != "0" && parsed != "false" && parsed != "FALSE" && parsed != "off" && parsed != "OFF";
}

bool clientKeeperCursorDrainEnabled()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN");
    if(value == nullptr || *value == '\0')
    {
        return false;
    }

    std::string const parsed(value);
    return parsed != "0" && parsed != "false" && parsed != "FALSE" && parsed != "off" && parsed != "OFF";
}

std::size_t clientKeeperCursorDrainMaxBatches()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_CURSOR_DRAIN_MAX_BATCHES");
    if(value == nullptr || *value == '\0')
    {
        return 0;
    }

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if(end == value)
    {
        return 0;
    }
    return static_cast<std::size_t>(parsed);
}

bool clientKeeperCursorPackedBatchEnabled()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BATCH");
    if(value == nullptr || *value == '\0')
    {
        return false;
    }

    std::string const parsed(value);
    return parsed != "0" && parsed != "false" && parsed != "FALSE" && parsed != "off" && parsed != "OFF";
}

bool clientKeeperCursorMetadataOnlyOutputEnabled()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_CURSOR_METADATA_ONLY_OUTPUT");
    if(value == nullptr || *value == '\0')
    {
        return false;
    }

    std::string const parsed(value);
    return parsed != "0" && parsed != "false" && parsed != "FALSE" && parsed != "off" && parsed != "OFF";
}

bool clientKeeperCursorPackedBulkEnabled()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK");
    if(value == nullptr || *value == '\0')
    {
        return false;
    }

    std::string const parsed(value);
    return parsed != "0" && parsed != "false" && parsed != "FALSE" && parsed != "off" && parsed != "OFF";
}

bool clientKeeperCursorPackedBulkStreamEnabled()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM");
    if(value == nullptr || *value == '\0')
    {
        return false;
    }

    std::string const parsed(value);
    return parsed != "0" && parsed != "false" && parsed != "FALSE" && parsed != "off" && parsed != "OFF";
}

std::size_t clientKeeperCursorPackedBulkStreamMaxBatches()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_STREAM_MAX_BATCHES");
    if(value == nullptr || *value == '\0')
    {
        return 0;
    }

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if(end == value)
    {
        return 0;
    }
    return static_cast<std::size_t>(parsed);
}

std::size_t clientKeeperCursorPackedBulkBufferBytes()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_KEEPER_CURSOR_PACKED_BULK_BUFFER_BYTES");
    if(value == nullptr || *value == '\0')
    {
        value = std::getenv("CHRONOLOG_KEEPER_TAIL_BATCH_MAX_BYTES");
    }
    if(value == nullptr || *value == '\0')
    {
        return 0;
    }

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if(end == value)
    {
        return 0;
    }
    return static_cast<std::size_t>(parsed);
}

bool clientBatchSingleKeeperEnabled()
{
    char const* value = std::getenv("CHRONOLOG_CLIENT_BATCH_KEEPER_SELECTION");
    if(value == nullptr || *value == '\0')
    {
        return false;
    }

    std::string const parsed(value);
    return parsed == "single_keeper" || parsed == "per_batch" || parsed == "1" || parsed == "true" ||
           parsed == "TRUE" || parsed == "on" || parsed == "ON" || parsed == "yes" || parsed == "YES";
}

uint64_t elapsedMicros(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

struct KeeperTailRequestResult
{
    int status{chronolog::CL_ERR_UNKNOWN};
    std::vector<chronolog::LogEvent> events;
};
}

/////////////////////

uint64_t chronolog::ChronologTimer::getTimestamp()
{
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

/////////////////////
chronolog::StoryHandle::~StoryHandle() {}

chronolog::LogEventFuture chronolog::StoryHandle::log_event_async(std::string const& event_record)
{
    return chronolog::LogEventFuture(std::make_shared<CompletedLogEventFutureState>(log_event(event_record)));
}

chronolog::LogEventFuture chronolog::StoryHandle::log_events_async(std::vector<std::string> const& event_records)
{
    if(event_records.empty())
    {
        return chronolog::LogEventFuture(std::make_shared<CompletedLogEventFutureState>(0));
    }
    if(event_records.size() == 1)
    {
        return log_event_async(event_records.front());
    }

    std::vector<chronolog::LogEventFuture> futures;
    futures.reserve(event_records.size());
    for(auto const& event_record: event_records)
    {
        futures.emplace_back(log_event_async(event_record));
    }
    return chronolog::LogEventFuture(std::make_shared<CompositeLogEventFutureState>(std::move(futures)));
}

chronolog::LogEventFuture chronolog::StoryHandle::log_events_async_owned(std::vector<std::string> event_records)
{
    return log_events_async(event_records);
}

uint64_t chronolog::StoryHandle::log_events_bounded(std::vector<std::string> const& event_records,
                                                    std::size_t batch_size,
                                                    std::size_t max_outstanding)
{
    CL_PROFILE_REGION("client_append_bounded");
    if(event_records.empty())
    {
        return 0;
    }

    batch_size = std::max<std::size_t>(batch_size, 1);
    max_outstanding = std::max<std::size_t>(max_outstanding, 1);

    auto wait_oldest = [](std::deque<chronolog::LogEventFuture>& pending) -> uint64_t {
        if(pending.empty())
        {
            return 0;
        }
        auto future = std::move(pending.front());
        pending.pop_front();
        return future.wait();
    };

    std::deque<chronolog::LogEventFuture> pending;
    std::vector<std::string> batch;
    batch.reserve(batch_size);
    uint64_t last_timestamp = 0;

    auto submit_batch = [&]() -> bool {
        if(batch.empty())
        {
            return true;
        }

        chronolog::LogEventFuture future = log_events_async_owned(std::move(batch));
        batch.clear();
        batch.reserve(batch_size);
        if(!future.valid())
        {
            return false;
        }
        pending.push_back(std::move(future));
        if(pending.size() >= max_outstanding)
        {
            last_timestamp = wait_oldest(pending);
            if(last_timestamp == 0)
            {
                return false;
            }
        }
        return true;
    };

    for(auto const& event_record: event_records)
    {
        batch.push_back(event_record);
        if(batch.size() >= batch_size && !submit_batch())
        {
            return 0;
        }
    }
    if(!submit_batch())
    {
        return 0;
    }
    while(!pending.empty())
    {
        last_timestamp = wait_oldest(pending);
        if(last_timestamp == 0)
        {
            return 0;
        }
    }
    return last_timestamp;
}

uint64_t chronolog::StoryHandle::log_events_bounded_per_keeper(std::vector<std::string> const& event_records,
                                                               std::size_t keeper_batch_size,
                                                               std::size_t max_outstanding_futures)
{
    return log_events_bounded(event_records, keeper_batch_size, max_outstanding_futures);
}

std::unique_ptr<chronolog::PerKeeperBoundedLogEventAppender> chronolog::StoryHandle::make_per_keeper_bounded_appender(
        std::size_t keeper_batch_size,
        std::size_t max_outstanding_futures)
{
    class FallbackPerKeeperAppender: public chronolog::PerKeeperBoundedLogEventAppender
    {
    public:
        FallbackPerKeeperAppender(chronolog::StoryHandle& handle,
                                  std::size_t keeper_batch_size,
                                  std::size_t max_outstanding_futures)
            : storyHandle(handle)
            , batchSize(std::max<std::size_t>(keeper_batch_size, 1))
            , maxOutstanding(std::max<std::size_t>(max_outstanding_futures, 1))
        {}

        uint64_t append(std::string const& event_record) override
        {
            begin_call();
            return storyHandle.log_events_bounded_per_keeper(std::vector<std::string>{event_record},
                                                             batchSize,
                                                             maxOutstanding);
        }

        uint64_t append_many(std::vector<std::string> event_records) override
        {
            begin_call();
            return storyHandle.log_events_bounded_per_keeper(event_records, batchSize, maxOutstanding);
        }

        uint64_t flush() override
        {
            begin_call();
            return 1;
        }

        [[nodiscard]] uint64_t future_count() const override { return futureCount; }
        [[nodiscard]] uint64_t future_count_max_per_call() const override { return futureCountMaxPerCall; }

    private:
        void begin_call() { futureCountMaxPerCall = std::max<uint64_t>(futureCountMaxPerCall, 0); }

        chronolog::StoryHandle& storyHandle;
        std::size_t batchSize;
        std::size_t maxOutstanding;
        uint64_t futureCount{0};
        uint64_t futureCountMaxPerCall{0};
    };

    return std::make_unique<FallbackPerKeeperAppender>(*this, keeper_batch_size, max_outstanding_futures);
}

chronolog::BoundedLogEventAppender::BoundedLogEventAppender(chronolog::StoryHandle& handle,
                                                            std::size_t requested_batch_size,
                                                            std::size_t requested_max_outstanding)
    : story_handle(handle)
    , batch_size(std::max<std::size_t>(requested_batch_size, 1))
    , max_outstanding(std::max<std::size_t>(requested_max_outstanding, 1))
{
    pending_batch.reserve(batch_size);
}

uint64_t chronolog::BoundedLogEventAppender::wait_oldest()
{
    if(pending_writes.empty())
    {
        return 0;
    }
    auto future = std::move(pending_writes.front());
    pending_writes.pop_front();
    last_timestamp = future.wait();
    return last_timestamp;
}

bool chronolog::BoundedLogEventAppender::submit_batch()
{
    if(pending_batch.empty())
    {
        return true;
    }

    chronolog::LogEventFuture future = story_handle.log_events_async_owned(std::move(pending_batch));
    pending_batch.clear();
    pending_batch.reserve(batch_size);
    if(!future.valid())
    {
        return false;
    }
    pending_writes.push_back(std::move(future));
    if(pending_writes.size() >= max_outstanding)
    {
        return wait_oldest() != 0;
    }
    return true;
}

uint64_t chronolog::BoundedLogEventAppender::append(std::string const& event_record)
{
    CL_PROFILE_REGION("client_append_bounded_stream");
    pending_batch.push_back(event_record);
    if(pending_batch.size() >= batch_size && !submit_batch())
    {
        return 0;
    }
    return last_timestamp == 0 ? 1 : last_timestamp;
}

uint64_t chronolog::BoundedLogEventAppender::append_many(std::vector<std::string> event_records)
{
    CL_PROFILE_REGION("client_append_bounded_stream_many");
    uint64_t timestamp = last_timestamp == 0 ? 1 : last_timestamp;
    for(auto& event_record: event_records)
    {
        pending_batch.push_back(std::move(event_record));
        if(pending_batch.size() >= batch_size && !submit_batch())
        {
            return 0;
        }
    }
    return timestamp;
}

uint64_t chronolog::BoundedLogEventAppender::flush()
{
    CL_PROFILE_REGION("client_append_bounded_stream_flush");
    if(!submit_batch())
    {
        return 0;
    }
    while(!pending_writes.empty())
    {
        if(wait_oldest() == 0)
        {
            return 0;
        }
    }
    return last_timestamp;
}

template <class KeeperChoicePolicy>
class chronolog::StoryWritingHandle<KeeperChoicePolicy>::PerKeeperAppender:
        public chronolog::PerKeeperBoundedLogEventAppender
{
public:
    PerKeeperAppender(chronolog::StoryWritingHandle<KeeperChoicePolicy>& writing_handle,
                      std::size_t requested_keeper_batch_size,
                      std::size_t requested_max_outstanding_futures)
        : handle(writing_handle)
        , keeperBatchSize(std::max<std::size_t>(requested_keeper_batch_size, 1))
        , maxOutstandingFutures(std::max<std::size_t>(requested_max_outstanding_futures, 1))
    {}

    uint64_t append(std::string const& event_record) override
    {
        CL_PROFILE_REGION("client_append_per_keeper_bounded_stream");
        begin_call();
        if(!append_one(event_record))
        {
            end_call();
            return 0;
        }
        end_call();
        return lastTimestamp == 0 ? 1 : lastTimestamp;
    }

    uint64_t append_many(std::vector<std::string> event_records) override
    {
        CL_PROFILE_REGION("client_append_per_keeper_bounded_stream_many");
        begin_call();
        for(auto& event_record: event_records)
        {
            if(!append_one(std::move(event_record)))
            {
                end_call();
                return 0;
            }
        }
        end_call();
        return lastTimestamp == 0 ? 1 : lastTimestamp;
    }

    uint64_t flush() override
    {
        CL_PROFILE_REGION("client_append_per_keeper_bounded_stream_flush");
        begin_call();
        for(auto& keeper_batch: batchesByKeeper)
        {
            if(!submit_batch(keeper_batch.first, keeper_batch.second))
            {
                end_call();
                return 0;
            }
        }
        while(!pendingFutures.empty())
        {
            if(!wait_oldest())
            {
                end_call();
                return 0;
            }
        }
        end_call();
        return lastTimestamp;
    }

    [[nodiscard]] uint64_t future_count() const override { return futureCount; }
    [[nodiscard]] uint64_t future_count_max_per_call() const override { return futureCountMaxPerCall; }
    [[nodiscard]] uint64_t future_wait_count() const override { return futureWaitCount; }
    [[nodiscard]] uint64_t future_wait_ns() const override { return futureWaitNs; }
    [[nodiscard]] uint64_t future_wait_max_ns() const override { return futureWaitMaxNs; }

private:
    struct PendingKeeperBatch
    {
        std::vector<chronolog::LogEvent> events;
        uint64_t payload_bytes{0};
        uint64_t event_build_ns{0};
        uint64_t keeper_select_ns{0};
    };

    template <class EventRecord>
    bool append_one(EventRecord&& event_record)
    {
        if(handle.storyKeepers.empty())
        {
            LOG_WARNING("[StoryWritingHandle] No keepers available for per-keeper bounded stream append");
            return false;
        }

        std::size_t const payload_size = event_record.size();
        uint64_t const build_start_ns = clientStatsNowNs();
        chronolog::LogEvent log_event(handle.storyId,
                                      handle.theClient.getTimestamp(),
                                      handle.theClient.getClientId(),
                                      handle.theClient.get_event_index(),
                                      std::forward<EventRecord>(event_record));
        uint64_t const event_build_ns = clientStatsNowNs() - build_start_ns;

        chronolog::KeeperRecordingClient* keeper_recording_client = nullptr;
        uint64_t keeper_select_ns = 0;
        {
            CL_PROFILE_REGION("client_keeper_select");
            uint64_t const select_start_ns = clientStatsNowNs();
            keeper_recording_client =
                    handle.keeperChoicePolicy->chooseKeeper(handle.storyKeepers, log_event.time());
            keeper_select_ns = clientStatsNowNs() - select_start_ns;
        }
        if(keeper_recording_client == nullptr)
        {
            LOG_WARNING("[StoryWritingHandle] No keeper selected for per-keeper bounded stream append");
            return false;
        }

        auto& batch = batchesByKeeper[keeper_recording_client];
        batch.payload_bytes += payload_size;
        batch.event_build_ns += event_build_ns;
        batch.keeper_select_ns += keeper_select_ns;
        batch.events.push_back(std::move(log_event));
        if(batch.events.size() >= keeperBatchSize)
        {
            return submit_batch(keeper_recording_client, batch);
        }
        return true;
    }

    bool wait_oldest()
    {
        if(pendingFutures.empty())
        {
            return true;
        }
        auto future = std::move(pendingFutures.front());
        pendingFutures.pop_front();
        uint64_t const wait_start_ns = clientStatsNowNs();
        lastTimestamp = future.wait();
        uint64_t const wait_ns = clientStatsNowNs() - wait_start_ns;
        futureWaitCount += 1;
        futureWaitNs += wait_ns;
        futureWaitMaxNs = std::max(futureWaitMaxNs, wait_ns);
        return lastTimestamp != 0;
    }

    bool submit_batch(chronolog::KeeperRecordingClient* keeper, PendingKeeperBatch& batch)
    {
        if(batch.events.empty())
        {
            return true;
        }

        CL_PROFILE_COUNTER("append_bytes", batch.payload_bytes);
        CL_PROFILE_COUNTER("append_batch_records", batch.events.size());
        CL_PROFILE_REGION("client_keeper_rpc_batch_async_submit");
        uint64_t const rpc_submit_start_ns = clientStatsNowNs();
        std::size_t const event_count = batch.events.size();
        uint64_t const payload_bytes = batch.payload_bytes;
        uint64_t const event_build_ns = batch.event_build_ns;
        uint64_t const keeper_select_ns = batch.keeper_select_ns;
        chronolog::LogEventFuture future = keeper->send_event_batch_msg_async(std::move(batch.events));
        batch.events.clear();
        batch.events.reserve(event_count);
        batch.payload_bytes = 0;
        batch.event_build_ns = 0;
        batch.keeper_select_ns = 0;
        ClientAppendStats::instance().recordBatch(event_count,
                                                  payload_bytes,
                                                  event_build_ns,
                                                  keeper_select_ns,
                                                  clientStatsNowNs() - rpc_submit_start_ns,
                                                  future.valid() ? 1 : 0);
        if(!future.valid())
        {
            return false;
        }
        pendingFutures.push_back(std::move(future));
        futureCount += 1;
        currentCallFutureCount += 1;
        while(pendingFutures.size() >= maxOutstandingFutures)
        {
            if(!wait_oldest())
            {
                return false;
            }
        }
        return true;
    }

    void begin_call() { currentCallFutureCount = 0; }

    void end_call()
    {
        futureCountMaxPerCall = std::max(futureCountMaxPerCall, currentCallFutureCount);
    }

    chronolog::StoryWritingHandle<KeeperChoicePolicy>& handle;
    std::size_t keeperBatchSize;
    std::size_t maxOutstandingFutures;
    std::map<chronolog::KeeperRecordingClient*, PendingKeeperBatch> batchesByKeeper;
    std::deque<chronolog::LogEventFuture> pendingFutures;
    uint64_t lastTimestamp{0};
    uint64_t futureCount{0};
    uint64_t futureCountMaxPerCall{0};
    uint64_t currentCallFutureCount{0};
    uint64_t futureWaitCount{0};
    uint64_t futureWaitNs{0};
    uint64_t futureWaitMaxNs{0};
};

////////////////////
template <class KeeperChoicePolicy>
// = chronolog::RoundRobinKeeperChoice>
chronolog::StoryWritingHandle<KeeperChoicePolicy>::~StoryWritingHandle()
{
    ClientAppendStats::instance().logSummary("story_handle_destroy");
    delete keeperChoicePolicy;
}

////////////////////
template <class KeeperChoicePolicy>
void chronolog::StoryWritingHandle<KeeperChoicePolicy>::addRecordingClient(
        chronolog::KeeperRecordingClient* keeperClient)
{
    storyKeepers.push_back(keeperClient);
    keeperTailCursors.emplace_back();
}

///////////////////
template <class KeeperChoicePolicy>
void chronolog::StoryWritingHandle<KeeperChoicePolicy>::removeRecordingClient(
        chronolog::ServiceId const& keeper_service_id)
{
    // this should only be called when the ChronoKeeper process unexpectedly exits
    // so it's ok to use rather inefficient vector iteration....
    for(auto iter = storyKeepers.begin(); iter != storyKeepers.end(); ++iter)
    {
        if((*iter)->getRecordingServiceId() == keeper_service_id)
        {
            auto const cursor_index = static_cast<std::size_t>(std::distance(storyKeepers.begin(), iter));
            storyKeepers.erase(iter);
            if(cursor_index < keeperTailCursors.size())
            {
                keeperTailCursors.erase(keeperTailCursors.begin() + static_cast<std::ptrdiff_t>(cursor_index));
            }
            break;
        }
    }
}

template <class KeeperChoicePolicy>
std::unique_ptr<chronolog::PerKeeperBoundedLogEventAppender>
chronolog::StoryWritingHandle<KeeperChoicePolicy>::make_per_keeper_bounded_appender(
        std::size_t keeper_batch_size,
        std::size_t max_outstanding_futures)
{
    return std::make_unique<PerKeeperAppender>(*this, keeper_batch_size, max_outstanding_futures);
}

//////////////////
template <class KeeperChoicePolicy>
uint64_t chronolog::StoryWritingHandle<KeeperChoicePolicy>::log_event(std::string const& event_record)
{
    CL_PROFILE_REGION("client_append");
    CL_PROFILE_COUNTER("append_bytes", event_record.size());

    chronolog::LogEvent log_event(storyId,
                                  theClient.getTimestamp(),
                                  theClient.getClientId(),
                                  theClient.get_event_index(),
                                  event_record);

    chronolog::KeeperRecordingClient* keeperRecordingClient = nullptr;
    {
        CL_PROFILE_REGION("client_keeper_select");
        keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, log_event.time());
    }
    if(nullptr == keeperRecordingClient) //very unlikely...
    {
        LOG_WARNING("[StoryWritingHandle] No keeper selected for logging event: {}", event_record);
        return 0;
    }

    int send_status = chronolog::CL_ERR_UNKNOWN;
    {
        CL_PROFILE_REGION("client_keeper_rpc");
        send_status = keeperRecordingClient->send_event_msg(log_event);
    }
    if(chronolog::CL_SUCCESS == send_status)
    {
        return log_event.eventTime;
    }
    else
    {
        return 0;
    }
}

template <class KeeperChoicePolicy>
chronolog::LogEventFuture chronolog::StoryWritingHandle<KeeperChoicePolicy>::log_event_async(
        std::string const& event_record)
{
    CL_PROFILE_REGION("client_append_async");
    CL_PROFILE_COUNTER("append_bytes", event_record.size());

    chronolog::LogEvent log_event(storyId,
                                  theClient.getTimestamp(),
                                  theClient.getClientId(),
                                  theClient.get_event_index(),
                                  event_record);

    chronolog::KeeperRecordingClient* keeperRecordingClient = nullptr;
    {
        CL_PROFILE_REGION("client_keeper_select");
        keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, log_event.time());
    }
    if(nullptr == keeperRecordingClient)
    {
        LOG_WARNING("[StoryWritingHandle] No keeper selected for asynchronously logging event: {}", event_record);
        return chronolog::LogEventFuture();
    }

    CL_PROFILE_REGION("client_keeper_rpc_async_submit");
    return keeperRecordingClient->send_event_msg_async(log_event);
}

template <class KeeperChoicePolicy>
chronolog::LogEventFuture chronolog::StoryWritingHandle<KeeperChoicePolicy>::log_events_async(
        std::vector<std::string> const& event_records)
{
    CL_PROFILE_REGION("client_append_batch_async");
    if(event_records.empty())
    {
        return chronolog::LogEventFuture(std::make_shared<CompletedLogEventFutureState>(0));
    }
    if(event_records.size() == 1)
    {
        return log_event_async(event_records.front());
    }
    if(storyKeepers.empty())
    {
        LOG_WARNING("[StoryWritingHandle] No keepers available for asynchronously logging event batch of size {}",
                    event_records.size());
        return chronolog::LogEventFuture();
    }

    if(clientBatchSingleKeeperEnabled())
    {
        std::vector<chronolog::LogEvent> batch_events;
        batch_events.reserve(event_records.size());
        uint64_t payload_bytes = 0;
        uint64_t first_event_time = 0;
        uint64_t event_build_ns = 0;
        uint64_t keeper_select_ns = 0;
        for(auto const& event_record: event_records)
        {
            payload_bytes += event_record.size();
            uint64_t const build_start_ns = clientStatsNowNs();
            chronolog::LogEvent log_event(storyId,
                                          theClient.getTimestamp(),
                                          theClient.getClientId(),
                                          theClient.get_event_index(),
                                          event_record);
            event_build_ns += clientStatsNowNs() - build_start_ns;
            if(first_event_time == 0)
            {
                first_event_time = log_event.time();
            }
            batch_events.push_back(std::move(log_event));
        }

        chronolog::KeeperRecordingClient* keeperRecordingClient = nullptr;
        {
            CL_PROFILE_REGION("client_keeper_select");
            uint64_t const select_start_ns = clientStatsNowNs();
            keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, first_event_time);
            keeper_select_ns += clientStatsNowNs() - select_start_ns;
        }
        if(nullptr == keeperRecordingClient)
        {
            LOG_WARNING("[StoryWritingHandle] No keeper selected for async batch");
            return chronolog::LogEventFuture();
        }

        CL_PROFILE_COUNTER("append_bytes", payload_bytes);
        CL_PROFILE_COUNTER("append_batch_records", event_records.size());
        CL_PROFILE_REGION("client_keeper_rpc_batch_async_submit");
        uint64_t const rpc_submit_start_ns = clientStatsNowNs();
        chronolog::LogEventFuture future = keeperRecordingClient->send_event_batch_msg_async(std::move(batch_events));
        ClientAppendStats::instance().recordBatch(event_records.size(),
                                                  payload_bytes,
                                                  event_build_ns,
                                                  keeper_select_ns,
                                                  clientStatsNowNs() - rpc_submit_start_ns,
                                                  future.valid() ? 1 : 0);
        return future;
    }

    std::map<chronolog::KeeperRecordingClient*, std::vector<chronolog::LogEvent>> batches_by_keeper;
    uint64_t payload_bytes = 0;
    uint64_t event_build_ns = 0;
    uint64_t keeper_select_ns = 0;
    for(auto const& event_record: event_records)
    {
        payload_bytes += event_record.size();
        uint64_t const build_start_ns = clientStatsNowNs();
        chronolog::LogEvent log_event(storyId,
                                      theClient.getTimestamp(),
                                      theClient.getClientId(),
                                      theClient.get_event_index(),
                                      event_record);
        event_build_ns += clientStatsNowNs() - build_start_ns;

        chronolog::KeeperRecordingClient* keeperRecordingClient = nullptr;
        {
            CL_PROFILE_REGION("client_keeper_select");
            uint64_t const select_start_ns = clientStatsNowNs();
            keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, log_event.time());
            keeper_select_ns += clientStatsNowNs() - select_start_ns;
        }
        if(nullptr == keeperRecordingClient)
        {
            LOG_WARNING("[StoryWritingHandle] No keeper selected for event in async batch");
            return chronolog::LogEventFuture();
        }
        batches_by_keeper[keeperRecordingClient].push_back(std::move(log_event));
    }
    CL_PROFILE_COUNTER("append_bytes", payload_bytes);
    CL_PROFILE_COUNTER("append_batch_records", event_records.size());

    std::vector<chronolog::LogEventFuture> futures;
    futures.reserve(batches_by_keeper.size());
    {
        CL_PROFILE_REGION("client_keeper_rpc_batch_async_submit");
        uint64_t const rpc_submit_start_ns = clientStatsNowNs();
        for(auto& batch: batches_by_keeper)
        {
            futures.emplace_back(batch.first->send_event_batch_msg_async(std::move(batch.second)));
        }
        ClientAppendStats::instance().recordBatch(event_records.size(),
                                                  payload_bytes,
                                                  event_build_ns,
                                                  keeper_select_ns,
                                                  clientStatsNowNs() - rpc_submit_start_ns,
                                                  futures.size());
    }
    return chronolog::LogEventFuture(std::make_shared<CompositeLogEventFutureState>(std::move(futures)));
}

template <class KeeperChoicePolicy>
chronolog::LogEventFuture chronolog::StoryWritingHandle<KeeperChoicePolicy>::log_events_async_owned(
        std::vector<std::string> event_records)
{
    CL_PROFILE_REGION("client_append_batch_async_owned");
    if(event_records.empty())
    {
        return chronolog::LogEventFuture(std::make_shared<CompletedLogEventFutureState>(0));
    }
    if(event_records.size() == 1)
    {
        return log_event_async(event_records.front());
    }
    if(storyKeepers.empty())
    {
        LOG_WARNING("[StoryWritingHandle] No keepers available for asynchronously logging owned event batch of size {}",
                    event_records.size());
        return chronolog::LogEventFuture();
    }

    if(clientBatchSingleKeeperEnabled())
    {
        std::vector<chronolog::LogEvent> batch_events;
        batch_events.reserve(event_records.size());
        uint64_t payload_bytes = 0;
        uint64_t first_event_time = 0;
        uint64_t event_build_ns = 0;
        uint64_t keeper_select_ns = 0;
        for(auto& event_record: event_records)
        {
            payload_bytes += event_record.size();
            uint64_t const build_start_ns = clientStatsNowNs();
            chronolog::LogEvent log_event(storyId,
                                          theClient.getTimestamp(),
                                          theClient.getClientId(),
                                          theClient.get_event_index(),
                                          std::move(event_record));
            event_build_ns += clientStatsNowNs() - build_start_ns;
            if(first_event_time == 0)
            {
                first_event_time = log_event.time();
            }
            batch_events.push_back(std::move(log_event));
        }

        chronolog::KeeperRecordingClient* keeperRecordingClient = nullptr;
        {
            CL_PROFILE_REGION("client_keeper_select");
            uint64_t const select_start_ns = clientStatsNowNs();
            keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, first_event_time);
            keeper_select_ns += clientStatsNowNs() - select_start_ns;
        }
        if(nullptr == keeperRecordingClient)
        {
            LOG_WARNING("[StoryWritingHandle] No keeper selected for owned async batch");
            return chronolog::LogEventFuture();
        }

        CL_PROFILE_COUNTER("append_bytes", payload_bytes);
        CL_PROFILE_COUNTER("append_batch_records", event_records.size());
        CL_PROFILE_REGION("client_keeper_rpc_batch_async_submit");
        uint64_t const rpc_submit_start_ns = clientStatsNowNs();
        chronolog::LogEventFuture future = keeperRecordingClient->send_event_batch_msg_async(std::move(batch_events));
        ClientAppendStats::instance().recordBatch(event_records.size(),
                                                  payload_bytes,
                                                  event_build_ns,
                                                  keeper_select_ns,
                                                  clientStatsNowNs() - rpc_submit_start_ns,
                                                  future.valid() ? 1 : 0);
        return future;
    }

    std::map<chronolog::KeeperRecordingClient*, std::vector<chronolog::LogEvent>> batches_by_keeper;
    uint64_t payload_bytes = 0;
    uint64_t event_build_ns = 0;
    uint64_t keeper_select_ns = 0;
    for(auto& event_record: event_records)
    {
        payload_bytes += event_record.size();
        uint64_t const build_start_ns = clientStatsNowNs();
        chronolog::LogEvent log_event(storyId,
                                      theClient.getTimestamp(),
                                      theClient.getClientId(),
                                      theClient.get_event_index(),
                                      std::move(event_record));
        event_build_ns += clientStatsNowNs() - build_start_ns;

        chronolog::KeeperRecordingClient* keeperRecordingClient = nullptr;
        {
            CL_PROFILE_REGION("client_keeper_select");
            uint64_t const select_start_ns = clientStatsNowNs();
            keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, log_event.time());
            keeper_select_ns += clientStatsNowNs() - select_start_ns;
        }
        if(nullptr == keeperRecordingClient)
        {
            LOG_WARNING("[StoryWritingHandle] No keeper selected for event in owned async batch");
            return chronolog::LogEventFuture();
        }
        batches_by_keeper[keeperRecordingClient].push_back(std::move(log_event));
    }
    CL_PROFILE_COUNTER("append_bytes", payload_bytes);
    CL_PROFILE_COUNTER("append_batch_records", event_records.size());

    std::vector<chronolog::LogEventFuture> futures;
    futures.reserve(batches_by_keeper.size());
    {
        CL_PROFILE_REGION("client_keeper_rpc_batch_async_submit");
        uint64_t const rpc_submit_start_ns = clientStatsNowNs();
        for(auto& batch: batches_by_keeper)
        {
            futures.emplace_back(batch.first->send_event_batch_msg_async(std::move(batch.second)));
        }
        ClientAppendStats::instance().recordBatch(event_records.size(),
                                                  payload_bytes,
                                                  event_build_ns,
                                                  keeper_select_ns,
                                                  clientStatsNowNs() - rpc_submit_start_ns,
                                                  futures.size());
    }
    return chronolog::LogEventFuture(std::make_shared<CompositeLogEventFutureState>(std::move(futures)));
}

template <class KeeperChoicePolicy>
uint64_t chronolog::StoryWritingHandle<KeeperChoicePolicy>::log_events_bounded_per_keeper(
        std::vector<std::string> const& event_records,
        std::size_t keeper_batch_size,
        std::size_t max_outstanding_futures)
{
    CL_PROFILE_REGION("client_append_bounded_per_keeper");
    if(event_records.empty())
    {
        return 0;
    }
    if(storyKeepers.empty())
    {
        LOG_WARNING("[StoryWritingHandle] No keepers available for per-keeper bounded batch of size {}",
                    event_records.size());
        return 0;
    }

    keeper_batch_size = std::max<std::size_t>(keeper_batch_size, 1);
    max_outstanding_futures = std::max<std::size_t>(max_outstanding_futures, 1);

    struct PendingKeeperBatch
    {
        std::vector<chronolog::LogEvent> events;
        uint64_t payload_bytes{0};
        uint64_t event_build_ns{0};
        uint64_t keeper_select_ns{0};
    };

    std::map<chronolog::KeeperRecordingClient*, PendingKeeperBatch> batches_by_keeper;
    std::deque<chronolog::LogEventFuture> pending_futures;
    uint64_t last_timestamp = 0;

    auto wait_oldest = [&]() -> bool {
        if(pending_futures.empty())
        {
            return true;
        }
        auto future = std::move(pending_futures.front());
        pending_futures.pop_front();
        last_timestamp = future.wait();
        return last_timestamp != 0;
    };

    auto submit_batch = [&](chronolog::KeeperRecordingClient* keeper, PendingKeeperBatch& batch) -> bool {
        if(batch.events.empty())
        {
            return true;
        }

        CL_PROFILE_COUNTER("append_bytes", batch.payload_bytes);
        CL_PROFILE_COUNTER("append_batch_records", batch.events.size());
        CL_PROFILE_REGION("client_keeper_rpc_batch_async_submit");
        uint64_t const rpc_submit_start_ns = clientStatsNowNs();
        std::size_t const event_count = batch.events.size();
        uint64_t const payload_bytes = batch.payload_bytes;
        uint64_t const event_build_ns = batch.event_build_ns;
        uint64_t const keeper_select_ns = batch.keeper_select_ns;
        chronolog::LogEventFuture future = keeper->send_event_batch_msg_async(std::move(batch.events));
        batch.events.clear();
        batch.events.reserve(event_count);
        batch.payload_bytes = 0;
        batch.event_build_ns = 0;
        batch.keeper_select_ns = 0;
        ClientAppendStats::instance().recordBatch(event_count,
                                                  payload_bytes,
                                                  event_build_ns,
                                                  keeper_select_ns,
                                                  clientStatsNowNs() - rpc_submit_start_ns,
                                                  future.valid() ? 1 : 0);
        if(!future.valid())
        {
            return false;
        }
        pending_futures.push_back(std::move(future));
        while(pending_futures.size() >= max_outstanding_futures)
        {
            if(!wait_oldest())
            {
                return false;
            }
        }
        return true;
    };

    for(auto const& event_record: event_records)
    {
        uint64_t const build_start_ns = clientStatsNowNs();
        chronolog::LogEvent log_event(storyId,
                                      theClient.getTimestamp(),
                                      theClient.getClientId(),
                                      theClient.get_event_index(),
                                      event_record);
        uint64_t const event_build_ns = clientStatsNowNs() - build_start_ns;

        chronolog::KeeperRecordingClient* keeperRecordingClient = nullptr;
        uint64_t keeper_select_ns = 0;
        {
            CL_PROFILE_REGION("client_keeper_select");
            uint64_t const select_start_ns = clientStatsNowNs();
            keeperRecordingClient = keeperChoicePolicy->chooseKeeper(storyKeepers, log_event.time());
            keeper_select_ns = clientStatsNowNs() - select_start_ns;
        }
        if(nullptr == keeperRecordingClient)
        {
            LOG_WARNING("[StoryWritingHandle] No keeper selected for event in per-keeper bounded batch");
            return 0;
        }

        auto& batch = batches_by_keeper[keeperRecordingClient];
        batch.payload_bytes += event_record.size();
        batch.event_build_ns += event_build_ns;
        batch.keeper_select_ns += keeper_select_ns;
        batch.events.push_back(std::move(log_event));
        if(batch.events.size() >= keeper_batch_size && !submit_batch(keeperRecordingClient, batch))
        {
            return 0;
        }
    }

    for(auto& keeper_batch: batches_by_keeper)
    {
        if(!submit_batch(keeper_batch.first, keeper_batch.second))
        {
            return 0;
        }
    }
    while(!pending_futures.empty())
    {
        if(!wait_oldest())
        {
            return 0;
        }
    }
    return last_timestamp;
}

template <class KeeperChoicePolicy>
int chronolog::StoryWritingHandle<KeeperChoicePolicy>::replay_tail(uint64_t start,
                                                                   uint64_t end,
                                                                   std::vector<chronolog::Event>& event_series)
{
    CL_PROFILE_REGION("client_query");
    CL_PROFILE_REGION("range_retrieval");
    CL_PROFILE_REGION("client_keeper_tail");

    if(storyKeepers.empty())
    {
        return chronolog::CL_ERR_NO_KEEPERS;
    }

    bool const replay_stats_enabled = clientReplayStatsEnabled();
    bool const parallel_tail_rpc = clientParallelTailRpcEnabled() && storyKeepers.size() > 1;
    auto const rpc_collect_start = std::chrono::steady_clock::now();
    std::vector<chronolog::LogEvent> tail_events;
    {
        CL_PROFILE_REGION("client_keeper_tail_rpc_collect");
        if(parallel_tail_rpc)
        {
            std::vector<std::future<KeeperTailRequestResult>> requests;
            requests.reserve(storyKeepers.size());
            for(auto* keeper: storyKeepers)
            {
                if(keeper == nullptr)
                {
                    continue;
                }
                requests.emplace_back(std::async(std::launch::async, [keeper, story_id = storyId, start, end]() {
                    KeeperTailRequestResult result;
                    result.status = keeper->send_tail_request(story_id, start, end, result.events);
                    return result;
                }));
            }

            for(auto& request: requests)
            {
                KeeperTailRequestResult result = request.get();
                if(result.status == chronolog::CL_SUCCESS)
                {
                    CL_PROFILE_COUNTER("client_keeper_tail_events", result.events.size());
                    tail_events.insert(tail_events.end(),
                                       std::make_move_iterator(result.events.begin()),
                                       std::make_move_iterator(result.events.end()));
                }
            }
        }
        else
        {
            for(auto* keeper: storyKeepers)
            {
                if(keeper == nullptr)
                {
                    continue;
                }

                std::vector<chronolog::LogEvent> keeper_events;
                if(keeper->send_tail_request(storyId, start, end, keeper_events) == chronolog::CL_SUCCESS)
                {
                    CL_PROFILE_COUNTER("client_keeper_tail_events", keeper_events.size());
                    tail_events.insert(tail_events.end(),
                                       std::make_move_iterator(keeper_events.begin()),
                                       std::make_move_iterator(keeper_events.end()));
                }
            }
        }
    }
    auto const rpc_collect_end = std::chrono::steady_clock::now();

    if(tail_events.empty())
    {
        return chronolog::CL_ERR_UNKNOWN;
    }

    auto const sort_start = std::chrono::steady_clock::now();
    {
        CL_PROFILE_REGION("client_keeper_tail_sort");
        std::sort(tail_events.begin(), tail_events.end(), [](auto const& lhs, auto const& rhs) {
            return std::tie(lhs.eventTime, lhs.clientId, lhs.eventIndex) <
                   std::tie(rhs.eventTime, rhs.clientId, rhs.eventIndex);
        });
    }
    auto const sort_end = std::chrono::steady_clock::now();

    std::size_t payload_bytes = 0;
    auto const output_start = std::chrono::steady_clock::now();
    {
        CL_PROFILE_REGION("client_keeper_tail_output_build");
        event_series.reserve(event_series.size() + tail_events.size());
        for(auto& log_event: tail_events)
        {
            payload_bytes += log_event.logRecord.size();
            event_series.emplace_back(log_event.eventTime,
                                      log_event.clientId,
                                      log_event.eventIndex,
                                      std::move(log_event.logRecord));
        }
    }
    auto const output_end = std::chrono::steady_clock::now();
    CL_PROFILE_COUNTER("client_keeper_tail_payload_bytes", payload_bytes);
    if(replay_stats_enabled)
    {
        uint64_t const rpc_collect_us = elapsedMicros(rpc_collect_start, rpc_collect_end);
        uint64_t const sort_us = elapsedMicros(sort_start, sort_end);
        uint64_t const output_build_us = elapsedMicros(output_start, output_end);
        LOG_INFO("[ClientReplayStats] mode={} story_id={} event_count={} payload_bytes={} rpc_collect_us={} "
                 "sort_us={} output_build_us={}",
                 parallel_tail_rpc ? "full_parallel" : "full",
                 storyId,
                 tail_events.size(),
                 payload_bytes,
                 rpc_collect_us,
                 sort_us,
                 output_build_us);
        std::cerr << "[ClientReplayStats] mode=" << (parallel_tail_rpc ? "full_parallel" : "full")
                  << " story_id=" << storyId << " event_count=" << tail_events.size()
                  << " payload_bytes=" << payload_bytes << " rpc_collect_us=" << rpc_collect_us
                  << " sort_us=" << sort_us << " output_build_us=" << output_build_us << std::endl;
    }

    return chronolog::CL_SUCCESS;
}

template <class KeeperChoicePolicy>
int chronolog::StoryWritingHandle<KeeperChoicePolicy>::replay_tail_incremental(
        uint64_t end,
        std::vector<chronolog::Event>& event_series)
{
    CL_PROFILE_REGION("client_query");
    CL_PROFILE_REGION("range_retrieval");
    CL_PROFILE_REGION("client_keeper_tail_incremental");

    if(storyKeepers.empty())
    {
        return chronolog::CL_ERR_NO_KEEPERS;
    }
    if(keeperTailCursors.size() != storyKeepers.size())
    {
        keeperTailCursors.resize(storyKeepers.size());
    }

    bool const replay_stats_enabled = clientReplayStatsEnabled();
    bool const parallel_tail_rpc = clientParallelTailRpcEnabled() && storyKeepers.size() > 1;
    bool const drain_cursor_batches = clientKeeperCursorDrainEnabled();
    std::size_t const drain_max_batches = clientKeeperCursorDrainMaxBatches();
    bool const packed_cursor_batch = clientKeeperCursorPackedBatchEnabled();
    bool const packed_direct_output = packed_cursor_batch && !drain_cursor_batches;
    bool const metadata_only_output = packed_direct_output && clientKeeperCursorMetadataOnlyOutputEnabled();
    auto const rpc_collect_start = std::chrono::steady_clock::now();
    std::vector<chronolog::LogEvent> tail_events;
    std::vector<chronolog::KeeperTailPackedBatch> packed_tail_batches;
    bool cursor_rpc_success = false;
    bool cursor_rpc_attempted = false;
    {
        CL_PROFILE_REGION("client_keeper_tail_rpc_collect");
        struct IncrementalTailRequestResult
        {
            std::size_t keeper_index{0};
            bool attempted{false};
            bool cursor_success{false};
            bool fallback_success{false};
            std::size_t raw_event_count{0};
            TailCursor next_cursor;
            std::vector<chronolog::LogEvent> events;
            chronolog::KeeperTailPackedBatch packed_events;
        };

        auto request_keeper = [this, end, drain_cursor_batches, drain_max_batches, packed_cursor_batch](
                                      std::size_t keeper_index) {
            IncrementalTailRequestResult result;
            result.keeper_index = keeper_index;
            auto* keeper = storyKeepers[keeper_index];
            if(keeper == nullptr)
            {
                return result;
            }

            TailCursor cursor = keeperTailCursors[keeper_index];
            result.next_cursor = cursor;
            auto send_cursor_request = [this, keeper, story_id = storyId, packed_cursor_batch](
                                               KeeperTailCursorToken const& cursor_token, KeeperTailBatch& output) {
                if(packed_cursor_batch)
                {
                    return keeper->send_tail_since_packed_request(story_id, cursor_token, output);
                }
                return keeper->send_tail_since_request(story_id, cursor_token, output);
            };
            KeeperTailBatch batch;
            result.attempted = true;
            if(packed_cursor_batch && !drain_cursor_batches)
            {
                KeeperTailPackedBatch packed_batch;
                if(keeper->send_tail_since_packed_raw_request(storyId, cursor.journalCursor, packed_batch) ==
                           chronolog::CL_SUCCESS &&
                   packed_batch.ok)
                {
                    std::size_t const count = packed_batch.eventTimes.size();
                    if(packed_batch.clientIds.size() != count || packed_batch.eventIndexes.size() != count ||
                       packed_batch.payloadOffsets.size() != count || packed_batch.payloadSizes.size() != count)
                    {
                        return result;
                    }
                    result.cursor_success = true;
                    result.next_cursor.journalCursor = packed_batch.nextCursor;
                    result.raw_event_count = count;
                    result.packed_events = std::move(packed_batch);
                    return result;
                }
            }
            if(drain_cursor_batches)
            {
                for(std::size_t batch_count = 0;; ++batch_count)
                {
                    KeeperTailBatch next_batch;
                    if(send_cursor_request(cursor.journalCursor, next_batch) != chronolog::CL_SUCCESS || !next_batch.ok)
                    {
                        break;
                    }
                    result.cursor_success = true;
                    result.next_cursor.journalCursor = next_batch.nextCursor;
                    std::size_t const event_count = next_batch.events.size();
                    result.raw_event_count += event_count;
                    result.events.insert(result.events.end(),
                                         std::make_move_iterator(next_batch.events.begin()),
                                         std::make_move_iterator(next_batch.events.end()));
                    cursor.journalCursor = next_batch.nextCursor;
                    if(event_count == 0 || (drain_max_batches != 0 && batch_count + 1 >= drain_max_batches))
                    {
                        return result;
                    }
                }
                if(result.cursor_success)
                {
                    return result;
                }
            }
            else if(send_cursor_request(cursor.journalCursor, batch) == chronolog::CL_SUCCESS && batch.ok)
            {
                result.cursor_success = true;
                result.next_cursor.journalCursor = batch.nextCursor;
                result.raw_event_count = batch.events.size();
                result.events = std::move(batch.events);
                return result;
            }

            uint64_t const overlap_ns = keeperTailCursorOverlapNs();
            uint64_t const start = cursor.initialized
                                           ? (cursor.eventTime > overlap_ns ? cursor.eventTime - overlap_ns : 1)
                                           : 1;
            std::vector<chronolog::LogEvent> keeper_events;
            if(keeper->send_tail_request(storyId, start, end, keeper_events) != chronolog::CL_SUCCESS)
            {
                return result;
            }

            TailCursor next_cursor = cursor;
            for(auto& event: keeper_events)
            {
                std::tuple<uint64_t, uint64_t, uint32_t> const key{event.eventTime, event.clientId, event.eventIndex};
                if(next_cursor.seenEvents.insert(key).second)
                {
                    if(!next_cursor.initialized || event.eventTime > next_cursor.eventTime)
                    {
                        next_cursor.eventTime = event.eventTime;
                        next_cursor.initialized = true;
                    }
                    result.events.push_back(std::move(event));
                }
            }
            result.fallback_success = true;
            result.raw_event_count = keeper_events.size();
            result.next_cursor = std::move(next_cursor);
            return result;
        };

        auto collect_result = [&](IncrementalTailRequestResult&& result) {
            if(!result.attempted)
            {
                return;
            }
            cursor_rpc_attempted = true;
            cursor_rpc_success = cursor_rpc_success || result.cursor_success;
            if(result.cursor_success || result.fallback_success)
            {
                keeperTailCursors[result.keeper_index] = std::move(result.next_cursor);
                CL_PROFILE_COUNTER("client_keeper_tail_events", result.raw_event_count);
                if(packed_direct_output && result.cursor_success)
                {
                    packed_tail_batches.push_back(std::move(result.packed_events));
                }
                else
                {
                    tail_events.insert(tail_events.end(),
                                       std::make_move_iterator(result.events.begin()),
                                       std::make_move_iterator(result.events.end()));
                }
            }
        };

        if(parallel_tail_rpc)
        {
            std::vector<std::future<IncrementalTailRequestResult>> requests;
            requests.reserve(storyKeepers.size());
            for(std::size_t keeper_index = 0; keeper_index < storyKeepers.size(); ++keeper_index)
            {
                if(storyKeepers[keeper_index] != nullptr)
                {
                    requests.emplace_back(std::async(std::launch::async, request_keeper, keeper_index));
                }
            }
            for(auto& request: requests)
            {
                collect_result(request.get());
            }
        }
        else
        {
            for(std::size_t keeper_index = 0; keeper_index < storyKeepers.size(); ++keeper_index)
            {
                collect_result(request_keeper(keeper_index));
            }
        }
    }
    auto const rpc_collect_end = std::chrono::steady_clock::now();

    std::size_t packed_event_count = 0;
    for(auto const& batch: packed_tail_batches)
    {
        packed_event_count += batch.eventTimes.size();
    }

    if(tail_events.empty() && packed_event_count == 0)
    {
        return cursor_rpc_attempted && cursor_rpc_success ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN;
    }

    auto const sort_start = std::chrono::steady_clock::now();
    struct PackedTailRef
    {
        std::size_t batch_index{0};
        std::size_t event_index{0};
    };
    std::vector<PackedTailRef> packed_tail_refs;
    {
        CL_PROFILE_REGION("client_keeper_tail_sort");
        if(packed_direct_output)
        {
            packed_tail_refs.reserve(packed_event_count);
            for(std::size_t batch_index = 0; batch_index < packed_tail_batches.size(); ++batch_index)
            {
                auto const& batch = packed_tail_batches[batch_index];
                for(std::size_t event_index = 0; event_index < batch.eventTimes.size(); ++event_index)
                {
                    packed_tail_refs.push_back(PackedTailRef{batch_index, event_index});
                }
            }
            std::sort(packed_tail_refs.begin(), packed_tail_refs.end(), [&](auto const& lhs, auto const& rhs) {
                auto const& lhs_batch = packed_tail_batches[lhs.batch_index];
                auto const& rhs_batch = packed_tail_batches[rhs.batch_index];
                return std::tie(lhs_batch.eventTimes[lhs.event_index],
                                lhs_batch.clientIds[lhs.event_index],
                                lhs_batch.eventIndexes[lhs.event_index]) <
                       std::tie(rhs_batch.eventTimes[rhs.event_index],
                                rhs_batch.clientIds[rhs.event_index],
                                rhs_batch.eventIndexes[rhs.event_index]);
            });
        }
        else
        {
            std::sort(tail_events.begin(), tail_events.end(), [](auto const& lhs, auto const& rhs) {
                return std::tie(lhs.eventTime, lhs.clientId, lhs.eventIndex) <
                       std::tie(rhs.eventTime, rhs.clientId, rhs.eventIndex);
            });
        }
    }
    auto const sort_end = std::chrono::steady_clock::now();

    std::size_t payload_bytes = 0;
    auto const output_start = std::chrono::steady_clock::now();
    {
        CL_PROFILE_REGION("client_keeper_tail_output_build");
        event_series.reserve(event_series.size() + tail_events.size() + packed_event_count);
        if(packed_direct_output)
        {
            for(auto const& ref: packed_tail_refs)
            {
                auto const& batch = packed_tail_batches[ref.batch_index];
                std::size_t const index = ref.event_index;
                uint64_t const offset = batch.payloadOffsets[index];
                uint64_t const size = batch.payloadSizes[index];
                if(offset > batch.payloadBlob.size() || size > batch.payloadBlob.size() - offset)
                {
                    return chronolog::CL_ERR_UNKNOWN;
                }
                payload_bytes += static_cast<std::size_t>(size);
                if(metadata_only_output)
                {
                    event_series.emplace_back(batch.eventTimes[index],
                                              batch.clientIds[index],
                                              batch.eventIndexes[index],
                                              std::string());
                }
                else
                {
                    event_series.emplace_back(batch.eventTimes[index],
                                              batch.clientIds[index],
                                              batch.eventIndexes[index],
                                              batch.payloadBlob.substr(static_cast<std::size_t>(offset),
                                                                       static_cast<std::size_t>(size)));
                }
            }
        }
        for(auto& log_event: tail_events)
        {
            payload_bytes += log_event.logRecord.size();
            event_series.emplace_back(log_event.eventTime,
                                      log_event.clientId,
                                      log_event.eventIndex,
                                      std::move(log_event.logRecord));
        }
    }
    auto const output_end = std::chrono::steady_clock::now();
    CL_PROFILE_COUNTER("client_keeper_tail_payload_bytes", payload_bytes);
    if(replay_stats_enabled)
    {
        uint64_t const rpc_collect_us = elapsedMicros(rpc_collect_start, rpc_collect_end);
        uint64_t const sort_us = elapsedMicros(sort_start, sort_end);
        uint64_t const output_build_us = elapsedMicros(output_start, output_end);
        std::string const mode = std::string(parallel_tail_rpc ? "incremental_parallel" : "incremental") +
                                 (drain_cursor_batches ? "_drain" : "") +
                                 (packed_cursor_batch ? "_packed" : "") +
                                 (metadata_only_output ? "_metadata_only_output" : "");
        LOG_INFO("[ClientReplayStats] mode={} story_id={} event_count={} payload_bytes={} "
                 "rpc_collect_us={} sort_us={} output_build_us={}",
                 mode,
                 storyId,
                 tail_events.size() + packed_event_count,
                 payload_bytes,
                 rpc_collect_us,
                 sort_us,
                 output_build_us);
        std::cerr << "[ClientReplayStats] mode=" << mode
                  << " story_id=" << storyId
                  << " event_count=" << tail_events.size() + packed_event_count << " payload_bytes=" << payload_bytes
                  << " rpc_collect_us=" << rpc_collect_us << " sort_us=" << sort_us
                  << " output_build_us=" << output_build_us << std::endl;
    }

    return chronolog::CL_SUCCESS;
}

template <class KeeperChoicePolicy>
int chronolog::StoryWritingHandle<KeeperChoicePolicy>::replay_tail_incremental_packed(
        uint64_t end,
        chronolog::PackedReplayBatch& batch)
{
    CL_PROFILE_REGION("client_query");
    CL_PROFILE_REGION("range_retrieval");
    CL_PROFILE_REGION("client_keeper_tail_incremental_packed");

    bool const replay_stats_enabled = clientReplayStatsEnabled();
    bool const parallel_tail_rpc = clientParallelTailRpcEnabled() && storyKeepers.size() > 1;
    bool const drain_cursor_batches = clientKeeperCursorDrainEnabled();
    std::size_t const drain_max_batches = clientKeeperCursorDrainMaxBatches();
    bool const packed_bulk_transfer = clientKeeperCursorPackedBulkEnabled();
    bool const packed_bulk_stream = packed_bulk_transfer && clientKeeperCursorPackedBulkStreamEnabled();
    std::size_t const packed_bulk_stream_max_batches = clientKeeperCursorPackedBulkStreamMaxBatches();
    std::size_t const packed_bulk_buffer_bytes = clientKeeperCursorPackedBulkBufferBytes();
    std::vector<std::string> reusable_payload_blobs;
    if(packed_bulk_transfer)
    {
        reusable_payload_blobs.reserve(batch.payloadBlobs.size());
        for(auto& blob: batch.payloadBlobs)
        {
            if(blob.capacity() > 0)
            {
                reusable_payload_blobs.push_back(std::move(blob));
            }
        }
    }
    batch.clear();
    if(storyKeepers.empty())
    {
        return chronolog::CL_ERR_NO_KEEPERS;
    }
    if(keeperTailCursors.size() != storyKeepers.size())
    {
        keeperTailCursors.resize(storyKeepers.size());
    }
    std::vector<std::string> reusable_receive_buffers(storyKeepers.size());
    if(packed_bulk_transfer)
    {
        std::size_t reusable_index = 0;
        for(std::size_t keeper_index = 0;
            keeper_index < reusable_receive_buffers.size() && reusable_index < reusable_payload_blobs.size();
            ++keeper_index)
        {
            if(storyKeepers[keeper_index] != nullptr)
            {
                reusable_receive_buffers[keeper_index] = std::move(reusable_payload_blobs[reusable_index++]);
            }
        }
        if(packed_bulk_buffer_bytes > 0)
        {
            for(std::size_t keeper_index = 0; keeper_index < reusable_receive_buffers.size(); ++keeper_index)
            {
                if(storyKeepers[keeper_index] != nullptr &&
                   reusable_receive_buffers[keeper_index].size() < packed_bulk_buffer_bytes)
                {
                    reusable_receive_buffers[keeper_index].resize(packed_bulk_buffer_bytes);
                }
            }
        }
    }
    auto const rpc_collect_start = std::chrono::steady_clock::now();

    struct PackedTailRequestResult
    {
        std::size_t keeper_index{0};
        bool attempted{false};
        bool cursor_success{false};
        std::size_t raw_event_count{0};
        std::size_t payload_bytes{0};
        std::size_t batch_count{0};
        uint64_t request_us{0};
        TailCursor next_cursor;
        std::vector<chronolog::KeeperTailPackedBatch> packed_batches;
    };

    auto request_keeper =
            [this,
             end,
             drain_cursor_batches,
             drain_max_batches,
             packed_bulk_transfer,
             packed_bulk_stream,
             packed_bulk_stream_max_batches,
             packed_bulk_buffer_bytes,
             &reusable_receive_buffers](
                    std::size_t keeper_index) {
        (void)end;
        PackedTailRequestResult result;
        result.keeper_index = keeper_index;
        auto* keeper = storyKeepers[keeper_index];
        if(keeper == nullptr)
        {
            return result;
        }

        TailCursor cursor = keeperTailCursors[keeper_index];
        result.next_cursor = cursor;
        result.attempted = true;
        auto const request_start = std::chrono::steady_clock::now();
        std::string receive_buffer =
                (packed_bulk_transfer && packed_bulk_buffer_bytes > 0)
                        ? std::move(reusable_receive_buffers[keeper_index])
                        : std::string();
        auto send_packed_request = [&](KeeperTailCursorToken const& cursor_token,
                                       chronolog::KeeperTailPackedBatch& output) {
            if(packed_bulk_transfer && packed_bulk_buffer_bytes > 0)
            {
                int const status =
                        packed_bulk_stream
                                ? keeper->send_tail_since_packed_bulk_stream_request(storyId,
                                                                                     cursor_token,
                                                                                     packed_bulk_buffer_bytes,
                                                                                     packed_bulk_stream_max_batches,
                                                                                     std::move(receive_buffer),
                                                                                     output)
                                : keeper->send_tail_since_packed_bulk_request(storyId,
                                                                              cursor_token,
                                                                              packed_bulk_buffer_bytes,
                                                                              std::move(receive_buffer),
                                                                              output);
                receive_buffer.clear();
                return status;
            }
            return keeper->send_tail_since_packed_raw_request(storyId, cursor_token, output);
        };

        bool first_request = true;
        for(std::size_t batch_index = 0;; ++batch_index)
        {
            chronolog::KeeperTailPackedBatch packed_batch;
            int const rpc_status = send_packed_request(cursor.journalCursor, packed_batch);
            if(rpc_status != chronolog::CL_SUCCESS || !packed_batch.ok)
            {
                break;
            }

            std::size_t const count = packed_batch.eventTimes.size();
            if(packed_batch.clientIds.size() != count || packed_batch.eventIndexes.size() != count ||
               packed_batch.payloadOffsets.size() != count || packed_batch.payloadSizes.size() != count)
            {
                break;
            }

            result.cursor_success = true;
            result.next_cursor.journalCursor = packed_batch.nextCursor;
            result.raw_event_count += count;
            result.payload_bytes += packed_batch.payloadBlob.size();
            result.batch_count += packed_batch.sourceBatchCount == 0 ? 1 : packed_batch.sourceBatchCount;
            cursor.journalCursor = packed_batch.nextCursor;
            result.packed_batches.push_back(std::move(packed_batch));

            first_request = false;
            if(!drain_cursor_batches || count == 0 || (drain_max_batches != 0 && batch_index + 1 >= drain_max_batches))
            {
                break;
            }
        }
        result.request_us = elapsedMicros(request_start, std::chrono::steady_clock::now());
        if(first_request && receive_buffer.capacity() > 0)
        {
            reusable_receive_buffers[keeper_index] = std::move(receive_buffer);
        }
        return result;
    };

    std::vector<chronolog::KeeperTailPackedBatch> packed_tail_batches;
    bool cursor_rpc_success = false;
    bool cursor_rpc_attempted = false;
    std::size_t request_attempt_count = 0;
    std::size_t request_success_count = 0;
    std::size_t request_event_count = 0;
    std::size_t request_payload_bytes = 0;
    std::size_t request_batch_count = 0;
    uint64_t request_sum_us = 0;
    uint64_t request_max_us = 0;
    auto collect_result = [&](PackedTailRequestResult&& result) {
        if(!result.attempted)
        {
            return;
        }
        cursor_rpc_attempted = true;
        cursor_rpc_success = cursor_rpc_success || result.cursor_success;
        ++request_attempt_count;
        request_sum_us += result.request_us;
        request_max_us = std::max(request_max_us, result.request_us);
        if(result.cursor_success)
        {
            ++request_success_count;
            request_event_count += result.raw_event_count;
            request_payload_bytes += result.payload_bytes;
            request_batch_count += result.batch_count;
            keeperTailCursors[result.keeper_index] = std::move(result.next_cursor);
            CL_PROFILE_COUNTER("client_keeper_tail_events", result.raw_event_count);
            for(auto& packed_events: result.packed_batches)
            {
                packed_tail_batches.push_back(std::move(packed_events));
            }
        }
    };

    if(parallel_tail_rpc)
    {
        std::vector<std::future<PackedTailRequestResult>> requests;
        requests.reserve(storyKeepers.size());
        for(std::size_t keeper_index = 0; keeper_index < storyKeepers.size(); ++keeper_index)
        {
            if(storyKeepers[keeper_index] != nullptr)
            {
                requests.emplace_back(std::async(std::launch::async, request_keeper, keeper_index));
            }
        }
        for(auto& request: requests)
        {
            collect_result(request.get());
        }
    }
    else
    {
        for(std::size_t keeper_index = 0; keeper_index < storyKeepers.size(); ++keeper_index)
        {
            collect_result(request_keeper(keeper_index));
        }
    }
    auto const rpc_collect_end = std::chrono::steady_clock::now();

    std::size_t packed_event_count = 0;
    std::size_t payload_bytes = 0;
    for(auto const& packed: packed_tail_batches)
    {
        packed_event_count += packed.eventTimes.size();
        payload_bytes += packed.payloadBlob.size();
    }
    if(packed_event_count == 0)
    {
        return cursor_rpc_attempted && cursor_rpc_success ? chronolog::CL_SUCCESS : chronolog::CL_ERR_UNKNOWN;
    }

    auto const sort_start = std::chrono::steady_clock::now();
    struct PackedTailRef
    {
        std::size_t batch_index{0};
        std::size_t event_index{0};
    };
    std::vector<PackedTailRef> packed_tail_refs;
    {
        CL_PROFILE_REGION("client_keeper_tail_sort");
        packed_tail_refs.reserve(packed_event_count);
        for(std::size_t batch_index = 0; batch_index < packed_tail_batches.size(); ++batch_index)
        {
            auto const& packed = packed_tail_batches[batch_index];
            for(std::size_t event_index = 0; event_index < packed.eventTimes.size(); ++event_index)
            {
                packed_tail_refs.push_back(PackedTailRef{batch_index, event_index});
            }
        }
        std::sort(packed_tail_refs.begin(), packed_tail_refs.end(), [&](auto const& lhs, auto const& rhs) {
            auto const& lhs_batch = packed_tail_batches[lhs.batch_index];
            auto const& rhs_batch = packed_tail_batches[rhs.batch_index];
            return std::tie(lhs_batch.eventTimes[lhs.event_index],
                            lhs_batch.clientIds[lhs.event_index],
                            lhs_batch.eventIndexes[lhs.event_index]) <
                   std::tie(rhs_batch.eventTimes[rhs.event_index],
                            rhs_batch.clientIds[rhs.event_index],
                            rhs_batch.eventIndexes[rhs.event_index]);
        });
    }
    auto const sort_end = std::chrono::steady_clock::now();

    auto const output_start = std::chrono::steady_clock::now();
    {
        CL_PROFILE_REGION("client_keeper_tail_output_build");
        batch.eventTimes.reserve(packed_event_count);
        batch.clientIds.reserve(packed_event_count);
        batch.eventIndexes.reserve(packed_event_count);
        batch.blobIndexes.reserve(packed_event_count);
        batch.payloadOffsets.reserve(packed_event_count);
        batch.payloadSizes.reserve(packed_event_count);
        batch.payloadBlobs.reserve(packed_tail_batches.size());
        for(auto& packed: packed_tail_batches)
        {
            batch.payloadBlobs.push_back(std::move(packed.payloadBlob));
        }
        for(auto const& ref: packed_tail_refs)
        {
            auto const& packed = packed_tail_batches[ref.batch_index];
            std::size_t const index = ref.event_index;
            batch.eventTimes.push_back(packed.eventTimes[index]);
            batch.clientIds.push_back(packed.clientIds[index]);
            batch.eventIndexes.push_back(packed.eventIndexes[index]);
            batch.blobIndexes.push_back(static_cast<uint32_t>(ref.batch_index));
            batch.payloadOffsets.push_back(packed.payloadOffsets[index]);
            batch.payloadSizes.push_back(packed.payloadSizes[index]);
        }
    }
    auto const output_end = std::chrono::steady_clock::now();
    CL_PROFILE_COUNTER("client_keeper_tail_payload_bytes", payload_bytes);

    if(replay_stats_enabled)
    {
        uint64_t const rpc_collect_us = elapsedMicros(rpc_collect_start, rpc_collect_end);
        uint64_t const sort_us = elapsedMicros(sort_start, sort_end);
        uint64_t const output_build_us = elapsedMicros(output_start, output_end);
        std::string const mode = std::string(parallel_tail_rpc ? "incremental_parallel" : "incremental") +
                                 "_packed_api" + (packed_bulk_transfer ? "_bulk" : "") +
                                 (packed_bulk_stream ? "_stream" : "") +
                                 (drain_cursor_batches ? "_drain" : "");
        LOG_INFO("[ClientReplayStats] mode={} story_id={} event_count={} payload_bytes={} "
                 "rpc_collect_us={} sort_us={} output_build_us={} request_attempt_count={} "
                 "request_success_count={} request_event_count={} request_payload_bytes={} "
                 "request_batch_count={} request_sum_us={} request_max_us={}",
                 mode,
                 storyId,
                 batch.event_count(),
                 payload_bytes,
                 rpc_collect_us,
                 sort_us,
                 output_build_us,
                 request_attempt_count,
                 request_success_count,
                 request_event_count,
                 request_payload_bytes,
                 request_batch_count,
                 request_sum_us,
                 request_max_us);
        std::cerr << "[ClientReplayStats] mode=" << mode
                  << " story_id=" << storyId
                  << " event_count=" << batch.event_count() << " payload_bytes=" << payload_bytes
                  << " rpc_collect_us=" << rpc_collect_us << " sort_us=" << sort_us
                  << " output_build_us=" << output_build_us
                  << " request_attempt_count=" << request_attempt_count
                  << " request_success_count=" << request_success_count
                  << " request_event_count=" << request_event_count
                  << " request_payload_bytes=" << request_payload_bytes
                  << " request_batch_count=" << request_batch_count
                  << " request_sum_us=" << request_sum_us
                  << " request_max_us=" << request_max_us << std::endl;
    }

    return chronolog::CL_SUCCESS;
}
/////////////////////

chronolog::StorytellerClient::~StorytellerClient()
{
    LOG_DEBUG("[StorytellerClient] Destructor called.");
    {
        std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);
        /*
        //TODO: investigate why the folowing lines were commented out in the previous version
        for( auto story_record_iter : acquiredStoryHandles)
        {
            delete story_record_iter.second;
        }
        acquiredStoryHandles.clear();
  */  }
        // stop & delete keeperRecordingClients
        std::lock_guard<std::mutex> lock(recordingClientMapMutex);
        for(auto keeper_client: recordingClientMap) { delete keeper_client.second; }
        recordingClientMap.clear();
}

int chronolog::StorytellerClient::get_event_index()
{
    // we only aqcuire mutex in the rare case when
    // the atomic index has reached INT_MAX value
    // otherwise proceed lock-free
    if((atomic_index == INT_MAX))
    {
        std::lock_guard<std::mutex> lock(recordingClientMapMutex);
        //recheck when mutex is acquired, only one thread changes the value
        if(atomic_index == INT_MAX)
        {
            atomic_index = 0;
        }
    }
    return ++atomic_index;
}
////////////////


int chronolog::StorytellerClient::addKeeperRecordingClient(chronolog::ServiceId const& keeper_service_id)
{
    std::lock_guard<std::mutex> lock(recordingClientMapMutex);

    try
    {
        chronolog::KeeperRecordingClient* keeperRecordingClient =
                chronolog::KeeperRecordingClient::CreateKeeperRecordingClient(client_engine, keeper_service_id);

        auto insert_return =
                recordingClientMap.insert(std::pair<std::pair<uint32_t, uint16_t>, chronolog::KeeperRecordingClient*>(
                        keeper_service_id.get_service_endpoint(),
                        keeperRecordingClient));
        if(false == insert_return.second)
        {
            LOG_ERROR("[StorytellerClient] Failed to create KeeperRecordingClient for {}",
                      to_string(keeper_service_id));
            return 0;
        }
        LOG_INFO("[StorytellerClient] Added KeeperRecordingClient for {}", to_string(keeper_service_id));
    }
    catch(tl::exception const& ex)
    {
        LOG_ERROR("[StorytellerClient] Failed to create KeeperRecordingClient for {}", to_string(keeper_service_id));
    }

    // state = RUNNING;
    LOG_INFO("[StorytellerClient] RUNNING with {} KeeperRecordingClients", recordingClientMap.size());
    return 1;
}
/////////////////

int chronolog::StorytellerClient::removeKeeperRecordingClient(chronolog::ServiceId const& keeper_service_id)
{
    std::lock_guard<std::mutex> lock(recordingClientMapMutex);

    // stop & delete keeperRecordingClient before erasing keeper_process entry
    auto keeper_client_iter = recordingClientMap.find(keeper_service_id.get_service_endpoint());

    if(keeper_client_iter != recordingClientMap.end())
    {
        delete(*keeper_client_iter).second;
        recordingClientMap.erase(keeper_client_iter);
    }

    //INNA: TODO: if this function is triggered by the Vizor calls when the ChronoKeeper process unexpectedly unregistered/exited
    // we need to iterate through the known WritingHandles and make sure this keeperClient is removed from all the active storyHandles
    // serialize the log events by switching the state to PENDING and forcing the log event calls to wait by locking
    //recording clientMutex during this time ....
    LOG_INFO("[StorytellerClient] Removed KeeperRecordingClient for {}", to_string(keeper_service_id));
    return 1;
}

///////////////////////////
chronolog::StoryHandle* chronolog::StorytellerClient::findStoryWritingHandle(ChronicleName const& chronicle,
                                                                             StoryName const& story)
{
    std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);

    auto story_record_iter = acquiredStoryHandles.find(std::pair<std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        LOG_DEBUG("[StorytellerClient::findStoryWritingHandle] Found StoryHandle for Chronicle: '{}' and Story: '{}'.",
                  chronicle,
                  story);
        return ((*story_record_iter).second);
    }
    else
    {
        LOG_DEBUG("[StorytellerClient::findStoryWritingHandle] StoryHandle not found for Chronicle: '{}' and Story: "
                  "'{}'.",
                  chronicle,
                  story);
        return (nullptr);
    }
}

/////////////

chronolog::StoryHandle*
chronolog::StorytellerClient::initializeStoryWritingHandle(ChronicleName const& chronicle,
                                                           StoryName const& story,
                                                           StoryId const& story_id,
                                                           std::vector<ServiceId> const& vectorOfKeepers,
                                                           chl::ServiceId const& player_card)
//INNA: TODO :KeeperChoicePolicy will have to be communicated here as well ....
{
    std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);

    auto story_record_iter = acquiredStoryHandles.find(std::pair<std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        LOG_DEBUG("[StorytellerClient] StoryHandle already exists for Chronicle: '{}' and Story: '{}'.",
                  chronicle,
                  story);
        return story_record_iter->second;
    }

    // create new StoryWritingHandle & initialize it's keeperClients vector
    chronolog::StoryWritingHandle<RoundRobinKeeperChoice>* storyWritingHandle =
            new StoryWritingHandle<RoundRobinKeeperChoice>(*this, chronicle, story, story_id);

    for(ServiceId const& keeper_service_id: vectorOfKeepers)
    {
        auto keeper_client_iter = recordingClientMap.find(keeper_service_id.get_service_endpoint());

        if(keeper_client_iter == recordingClientMap.end())
        {
            // unlikely but we better check
            if(addKeeperRecordingClient(keeper_service_id) == 0)
            {
                LOG_WARNING("[StorytellerClient] Failed to add KeeperRecordingClient for {}",
                            to_string(keeper_service_id));
                continue;
            }
        }
        keeper_client_iter = recordingClientMap.find(keeper_service_id.get_service_endpoint());

        storyWritingHandle->addRecordingClient((*keeper_client_iter).second);
    }

    auto insert_return =
            acquiredStoryHandles.insert(std::pair<std::pair<std::string, std::string>, chronolog::StoryHandle*>(
                    std::pair<std::string, std::string>(chronicle, story),
                    storyWritingHandle));
    if(!insert_return.second)
    {
        LOG_ERROR("[StorytellerClient] Failed to insert StoryWritingHandle for Chronicle: '{}' and Story: '{}'.",
                  chronicle,
                  story);
        delete storyWritingHandle;
        return nullptr;
    }

    LOG_INFO("[StorytellerClient] Successfully initialized StoryWritingHandle for Chronicle: '{}' and Story: '{}'.",
             chronicle,
             story);
    return storyWritingHandle;
    /*
    // now check the state of the handle:
    // it's possible the other thread is still pending the acquisition response from the Vizor,
    // or the handle's keeper vector is being updated , etc ....
    if (state == PENDING_RESPONSE || state== UPDATING_KEEPERS) )
    {
    // get the handle lock and wait for the thread that sent the request to Vizor to get the response
        std::lock_guard<std::mutex> story_lock(storyHandleMutex);

    }
    */
}

//////////////////////
void chronolog::StorytellerClient::removeAcquiredStoryHandle(ChronicleName const& chronicle, StoryName const& story)
{
    std::lock_guard<std::mutex> lock(acquiredStoryMapMutex);

    auto story_record_iter = acquiredStoryHandles.find(std::pair<std::string, std::string>(chronicle, story));
    if(story_record_iter != acquiredStoryHandles.end())
    {
        delete(*story_record_iter).second;
        acquiredStoryHandles.erase(story_record_iter);
        LOG_INFO("[StorytellerClient] Successfully removed StoryHandle for Chronicle: '{}' and Story: '{}'.",
                 chronicle,
                 story);
    }
    else
    {
        LOG_WARNING("[StorytellerClient] No matching StoryHandle found for Chronicle: '{}' and Story: '{}'.",
                    chronicle,
                    story);
    }
}

/////////////////
