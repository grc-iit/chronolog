#ifndef KEEPER_RECORDING_SERVICE_H
#define KEEPER_RECORDING_SERVICE_H

#include <atomic>
#include <chrono>
#include <iostream>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <cstdint>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/tuple.hpp>

#include <chronolog_errcode.h>
#include <KeeperIdCard.h>
#include <chronolog_types.h>
#include <HotRangeResponse.h>

#include "IngestionQueue.h"
#include "KeeperChunkRetentionStore.h"

namespace tl = thallium;

namespace chronolog
{
// Answers a durable write: KeeperDurableWriter::write in the keeper. Passed as
// a function so this header, and a test that runs the service, need neither
// the writer nor the WAL and the data store it calls.
using DurableWriteFunction = std::function<int(ChronicleName const&, StoryName const&, LogEvent const&)>;

class KeeperRecordingService: public tl::provider<KeeperRecordingService>
{
public:
    // KeeperRecordingService should be created on the heap not the stack thus the constructor is private...
    static KeeperRecordingService* CreateKeeperRecordingService(tl::engine& tl_engine,
                                                                uint16_t service_provider_id,
                                                                IngestionQueue& ingestion_queue,
                                                                KeeperChunkRetentionStore& tail_store,
                                                                DurableWriteFunction durable_write)
    {
        return new KeeperRecordingService(tl_engine,
                                          service_provider_id,
                                          ingestion_queue,
                                          tail_store,
                                          std::move(durable_write));
    }

    ~KeeperRecordingService()
    {
        LOG_DEBUG("[KeeperRecordingService] Destructor called. Cleaning up...");
        // thallium's provider does not deregister its RPCs: without this a
        // request after the delete runs a handler on the freed object
        for(tl::remote_procedure* rpc:
            {&recordEventRpc, &recordEventDurableRpc, &tailGetSequencesRpc, &tailGetEventsRpc, &storyRangeFetchRpc})
        {
            rpc->deregister();
        }
        get_engine().pop_finalize_callback(this);
    }

    // Shutdown, first step: take no new writes. A new durable write is
    // answered CL_ERR_DURABILITY_UNAVAILABLE and an accepted one CL_ERR_UNKNOWN,
    // both with nothing stored, instead of being taken after data collection
    // stopped. The RPCs stay registered: a handler already running, such as a
    // durable one waiting for its fdatasync, could not send its answer for an
    // RPC that is gone. So the service must outlive those handlers: delete it
    // only after waitForWriteHandlers.
    void stopAcceptingWrites() { acceptingWrites = false; }

    // Shutdown, second step, after the WAL has answered every queued durable
    // write: returns true once no write handler has been running for `quiet`
    // (so a request that arrived just before stopAcceptingWrites has started
    // and finished), false if handlers are still running after `max_wait`.
    bool waitForWriteHandlers(std::chrono::milliseconds quiet, std::chrono::milliseconds max_wait) const
    {
        auto const deadline = std::chrono::steady_clock::now() + max_wait;
        auto idle_since = std::chrono::steady_clock::now();
        while(std::chrono::steady_clock::now() < deadline)
        {
            auto const now = std::chrono::steady_clock::now();
            if(activeWriteHandlers.load() != 0)
            {
                idle_since = now;
            }
            else if(now - idle_since >= quiet)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return activeWriteHandlers.load() == 0;
    }

    void record_event(tl::request const& request, LogEvent const& log_event)
    {
        WriteHandlerGuard guard(activeWriteHandlers);
        //  ClientId teller_id,  StoryId story_id,
        //  ChronoTick const& chrono_tick, std::string const& record)
        LOG_TRACE("[KeeperRecordingService] Recording event: storyId={}, time={}, clientId={}, index={}, record={}",
                  log_event.getStoryId(),
                  log_event.time(),
                  log_event.getClientId(),
                  log_event.index(),
                  log_event.getRecord());
        if(!acceptingWrites)
        {
            request.respond(chronolog::CL_ERR_UNKNOWN);
            return;
        }
        theIngestionQueue.ingestLogEvent(log_event);
        request.respond(chronolog::CL_SUCCESS);
    }

    // A durable write: answered CL_SUCCESS only once the event is in the
    // keeper's WAL and its group's fdatasync returned, and ingested only then
    // (see KeeperDurableWriter). The handler's ULT yields while it waits, so
    // the RPC streams keep serving other requests. Defined whether the WAL is
    // on or off; with it off the answer is CL_ERR_DURABILITY_UNAVAILABLE.
    void record_event_durable(tl::request const& request,
                              ChronicleName const& chronicle,
                              StoryName const& story,
                              LogEvent const& log_event)
    {
        WriteHandlerGuard guard(activeWriteHandlers);
        LOG_TRACE("[KeeperRecordingService] Recording durable event: Chronicle={}, Story={}, storyId={}, time={}, "
                  "clientId={}, index={}",
                  chronicle,
                  story,
                  log_event.getStoryId(),
                  log_event.time(),
                  log_event.getClientId(),
                  log_event.index());
        if(!acceptingWrites)
        {
            request.respond(chronolog::CL_ERR_DURABILITY_UNAVAILABLE);
            return;
        }
        request.respond(theDurableWrite(chronicle, story, log_event));
    }

    // Tail-read phase 1: return this keeper's most recent (up to n) event
    // sequences for the story (cheap keys only, no payload).
    void tail_get_sequences(tl::request const& request, StoryId const& story_id, uint64_t n)
    {
        request.respond(theTailStore.getTailSequences(story_id, (std::size_t)n));
    }

    // Tail-read phase 2: return the payloads this keeper holds for the requested
    // event sequences (the globally-selected last-N, after client-side merge).
    void tail_get_events(tl::request const& request, StoryId const& story_id, std::vector<EventSequence> const& seqs)
    {
        request.respond(theTailStore.getTailEvents(story_id, seqs));
    }

    // Replay hot fetch: every retained event in [start, end) plus this
    // keeper's hot floor and last-seen persisted watermark. The player fans
    // this out over the story's keeper roster and splits the replay with
    // splitHotRange.
    void story_range_fetch(tl::request const& request,
                           StoryId const& story_id,
                           uint64_t start_time,
                           uint64_t end_time,
                           uint64_t max_events)
    {
        HotRangeResponse response = theTailStore.fetchRange(story_id, start_time, end_time, (std::size_t)max_events);
        LOG_DEBUG("[KeeperRecordingService] story_range_fetch StoryId={} {}-{} -> {} event(s), hot_floor={}, "
                  "known_W={}, truncated={}",
                  story_id,
                  start_time,
                  end_time,
                  response.events.size(),
                  response.hot_floor,
                  response.known_W,
                  response.truncated);
        request.respond(response);
    }

private:
    KeeperRecordingService(tl::engine& tl_engine,
                           uint16_t service_provider_id,
                           IngestionQueue& ingestion_queue,
                           KeeperChunkRetentionStore& tail_store,
                           DurableWriteFunction durable_write)
        : tl::provider<KeeperRecordingService>(tl_engine, service_provider_id)
        , theIngestionQueue(ingestion_queue)
        , theTailStore(tail_store)
        , theDurableWrite(std::move(durable_write))
    {
        recordEventRpc = define("record_event", &KeeperRecordingService::record_event, tl::ignore_return_value());
        recordEventDurableRpc = define("record_event_durable",
                                       &KeeperRecordingService::record_event_durable,
                                       tl::ignore_return_value());
        tailGetSequencesRpc = define("tail_get_sequences", &KeeperRecordingService::tail_get_sequences);
        tailGetEventsRpc = define("tail_get_events", &KeeperRecordingService::tail_get_events);
        storyRangeFetchRpc = define("story_range_fetch", &KeeperRecordingService::story_range_fetch);
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]() { delete p; });
    }

    KeeperRecordingService(KeeperRecordingService const&) = delete;

    KeeperRecordingService& operator=(KeeperRecordingService const&) = delete;

    // counts a write handler for as long as it runs (see waitForWriteHandlers)
    struct WriteHandlerGuard
    {
        explicit WriteHandlerGuard(std::atomic<int>& count)
            : theCount(count)
        {
            ++theCount;
        }
        ~WriteHandlerGuard() { --theCount; }
        std::atomic<int>& theCount;
    };

    IngestionQueue& theIngestionQueue;
    KeeperChunkRetentionStore& theTailStore;
    DurableWriteFunction theDurableWrite;
    tl::remote_procedure recordEventRpc;
    tl::remote_procedure recordEventDurableRpc;
    tl::remote_procedure tailGetSequencesRpc;
    tl::remote_procedure tailGetEventsRpc;
    tl::remote_procedure storyRangeFetchRpc;
    std::atomic<bool> acceptingWrites{true};
    std::atomic<int> activeWriteHandlers{0};
};

} // namespace chronolog

#endif
