#ifndef STORY_INGESTION_HANDLE_H
#define STORY_INGESTION_HANDLE_H

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <chronolog_types.h>
#include <chronolog_profile.h>

#include "KeeperAppendStats.h"

//
// IngestionQueue is a funnel into the KeeperDataStore
// std::deque guarantees O(1) time for addidng elements and resizing 
// (vector of vectors implementation)

namespace chronolog
{

typedef std::deque <LogEvent> EventDeque;

class StoryIngestionHandle
{

public:
    StoryIngestionHandle(std::mutex &a_mutex, EventDeque*active, EventDeque*passive): ingestionMutex(a_mutex)
                                                                                      , activeDeque(active)
                                                                                      , passiveDeque(passive)
                                                                                      , handleGeneration(nextHandleGeneration.fetch_add(1, std::memory_order_relaxed))
    {}

    ~StoryIngestionHandle() = default;

    EventDeque &getActiveDeque() const
    { return *activeDeque; }

    EventDeque &getPassiveDeque() const
    { return *passiveDeque; }

    void ingestEvent(LogEvent const& logEvent)
    {
        ingestEvent(LogEvent(logEvent));
    }

    void ingestEvent(LogEvent&& queuedEvent)
    {   // assume multiple service threads pushing events on ingestionQueue
        const uint64_t push_start_ns = KeeperAppendStats::nowNs();
        CL_PROFILE_REGION("keeper_queue_push");
        ProducerLane* lane = getProducerLane();
        pushToLane(*lane, std::move(queuedEvent));
        const uint64_t push_end_ns = KeeperAppendStats::nowNs();
        CL_PROFILE_COUNTER("keeper_active_queue_depth", lane->maxObservedDepth.load(std::memory_order_relaxed));
        KeeperAppendStats::instance().recordHandleIngest(0,
                                                         0,
                                                         push_end_ns - push_start_ns,
                                                         lane->maxObservedDepth.load(std::memory_order_relaxed));
    }

    void ingestEvents(std::vector<LogEvent>&& queuedEvents)
    {
        if(queuedEvents.empty())
        {
            return;
        }

        const uint64_t push_start_ns = KeeperAppendStats::nowNs();
        CL_PROFILE_REGION("keeper_queue_push_batch");
        ProducerLane* lane = getProducerLane();
        std::size_t pushed = 0;
        for(auto& event: queuedEvents)
        {
            pushToLane(*lane, std::move(event));
            ++pushed;
        }
        const uint64_t push_end_ns = KeeperAppendStats::nowNs();
        uint64_t const queue_depth = lane->maxObservedDepth.load(std::memory_order_relaxed);
        CL_PROFILE_COUNTER("keeper_active_queue_depth", queue_depth);
        CL_PROFILE_COUNTER("keeper_queue_push_batch_events", pushed);
        KeeperAppendStats::instance().recordHandleIngestBatch(0, 0, push_end_ns - push_start_ns, queue_depth, pushed);
    }

    void swapActiveDeque() //EventDeque * empty_deque, EventDeque * full_deque)
    {
        CL_PROFILE_REGION("keeper_ingestion_handle_collect_lanes");
        const uint64_t collect_wait_start_ns = KeeperAppendStats::nowNs();
        collectionMutex.lock();
        const uint64_t collect_wait_ns = KeeperAppendStats::nowNs() - collect_wait_start_ns;
        std::lock_guard<std::mutex> collect_lock(collectionMutex, std::adopt_lock);
        const uint64_t collect_hold_start_ns = KeeperAppendStats::nowNs();
        CL_PROFILE_REGION("keeper_queue_swap");
        DrainStats const drain_stats = drainProducerLanes(*passiveDeque);
        const uint64_t collect_hold_ns = KeeperAppendStats::nowNs() - collect_hold_start_ns;
        CL_PROFILE_COUNTER("keeper_queue_swap_events", passiveDeque->size());
        KeeperAppendStats::instance().recordHandleCollectLock(collect_wait_ns,
                                                              collect_hold_ns,
                                                              drain_stats.registryWaitNs,
                                                              drain_stats.registryHoldNs,
                                                              drain_stats.eventCount);
    }


private:
    struct ProducerLane
    {
        explicit ProducerLane(std::size_t requested_capacity)
            : buffer(requested_capacity)
            , capacity(requested_capacity)
        {}

        std::vector<LogEvent> buffer;
        std::size_t capacity;
        std::atomic<std::size_t> head{0};
        std::atomic<std::size_t> tail{0};
        std::atomic<std::size_t> maxObservedDepth{0};
    };

    struct DrainStats
    {
        uint64_t registryWaitNs{0};
        uint64_t registryHoldNs{0};
        uint64_t eventCount{0};
    };

    static constexpr std::size_t DEFAULT_LANE_CAPACITY = 32768;

    ProducerLane* getProducerLane()
    {
        static thread_local std::unordered_map<StoryIngestionHandle*, std::pair<uint64_t, ProducerLane*>>
                threadLocalLanes;
        auto lane_iter = threadLocalLanes.find(this);
        if(lane_iter != threadLocalLanes.end() && lane_iter->second.first == handleGeneration)
        {
            return lane_iter->second.second;
        }

        std::lock_guard<std::mutex> lock(laneRegistryMutex);
        producerLanes.emplace_back(std::make_unique<ProducerLane>(DEFAULT_LANE_CAPACITY));
        ProducerLane* lane = producerLanes.back().get();
        threadLocalLanes[this] = std::make_pair(handleGeneration, lane);
        CL_PROFILE_COUNTER("keeper_ingestion_lane_count", producerLanes.size());
        return lane;
    }

    static void updateMax(std::atomic<std::size_t>& target, std::size_t value)
    {
        std::size_t current = target.load(std::memory_order_relaxed);
        while(current < value && !target.compare_exchange_weak(current, value, std::memory_order_relaxed))
        {}
    }

    static std::size_t laneDepth(std::size_t head, std::size_t tail, std::size_t capacity)
    {
        return tail >= head ? tail - head : capacity - head + tail;
    }

    static void pushToLane(ProducerLane& lane, LogEvent&& event)
    {
        std::size_t tail = lane.tail.load(std::memory_order_relaxed);
        std::size_t next_tail = (tail + 1) % lane.capacity;
        while(next_tail == lane.head.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }

        lane.buffer[tail] = std::move(event);
        lane.tail.store(next_tail, std::memory_order_release);
        updateMax(lane.maxObservedDepth,
                  laneDepth(lane.head.load(std::memory_order_acquire), next_tail, lane.capacity));
    }

    DrainStats drainProducerLanes(EventDeque& destination)
    {
        DrainStats stats;
        const uint64_t registry_wait_start_ns = KeeperAppendStats::nowNs();
        laneRegistryMutex.lock();
        stats.registryWaitNs = KeeperAppendStats::nowNs() - registry_wait_start_ns;
        std::lock_guard<std::mutex> registry_lock(laneRegistryMutex, std::adopt_lock);
        const uint64_t registry_hold_start_ns = KeeperAppendStats::nowNs();
        for(auto& lane_ptr: producerLanes)
        {
            ProducerLane& lane = *lane_ptr;
            std::size_t head = lane.head.load(std::memory_order_relaxed);
            std::size_t tail = lane.tail.load(std::memory_order_acquire);
            while(head != tail)
            {
                destination.push_back(std::move(lane.buffer[head]));
                ++stats.eventCount;
                head = (head + 1) % lane.capacity;
                lane.head.store(head, std::memory_order_release);
                tail = lane.tail.load(std::memory_order_acquire);
            }
        }
        stats.registryHoldNs = KeeperAppendStats::nowNs() - registry_hold_start_ns;
        return stats;
    }

    std::mutex &ingestionMutex;
    EventDeque*activeDeque;
    EventDeque*passiveDeque;
    std::mutex collectionMutex;
    std::mutex laneRegistryMutex;
    std::vector<std::unique_ptr<ProducerLane>> producerLanes;
    uint64_t handleGeneration;
    inline static std::atomic<uint64_t> nextHandleGeneration{1};
};

}

#endif
