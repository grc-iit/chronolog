#ifndef KEEPER_RECORDING_SERVICE_H
#define KEEPER_RECORDING_SERVICE_H

#include <iostream>
#include <string>
#include <cstdint>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/tuple.hpp>

#include <chronolog_errcode.h>
#include <KeeperIdCard.h>
#include <chronolog_types.h>

#include "IngestionQueue.h"
#include "KeeperTailStore.h"

namespace tl = thallium;

namespace chronolog
{
class KeeperRecordingService: public tl::provider<KeeperRecordingService>
{
public:
    // KeeperRecordingService should be created on the heap not the stack thus the constructor is private...
    static KeeperRecordingService*
    CreateKeeperRecordingService(tl::engine& tl_engine, uint16_t service_provider_id, IngestionQueue& ingestion_queue,
                                 KeeperTailStore& tail_store)
    {
        return new KeeperRecordingService(tl_engine, service_provider_id, ingestion_queue, tail_store);
    }

    ~KeeperRecordingService()
    {
        LOG_DEBUG("[KeeperRecordingService] Destructor called. Cleaning up...");
        get_engine().pop_finalize_callback(this);
    }

    void record_event(tl::request const& request, LogEvent const& log_event)
    {
        //  ClientId teller_id,  StoryId story_id,
        //  ChronoTick const& chrono_tick, std::string const& record)
        LOG_TRACE("[KeeperRecordingService] Recording event: storyId={}, time={}, clientId={}, index={}, record={}",
                  log_event.getStoryId(),
                  log_event.time(),
                  log_event.getClientId(),
                  log_event.index(),
                  log_event.getRecord());
        theIngestionQueue.ingestLogEvent(log_event);
        request.respond(chronolog::CL_SUCCESS);
    }

    // Tail-read phase 1: return this keeper's most recent (up to n) event
    // sequences for the story (cheap keys only, no payload).
    void tail_get_sequences(tl::request const& request, StoryId const& story_id, uint64_t n)
    {
        request.respond(theTailStore.getTailSequences(story_id, (std::size_t)n));
    }

    // Tail-read phase 2: return the payloads this keeper holds for the requested
    // event sequences (the globally-selected last-N, after client-side merge).
    void tail_get_events(tl::request const& request, StoryId const& story_id,
                         std::vector<EventSequence> const& seqs)
    {
        request.respond(theTailStore.getTailEvents(story_id, seqs));
    }

private:
    KeeperRecordingService(tl::engine& tl_engine, uint16_t service_provider_id, IngestionQueue& ingestion_queue,
                           KeeperTailStore& tail_store)
        : tl::provider<KeeperRecordingService>(tl_engine, service_provider_id)
        , theIngestionQueue(ingestion_queue)
        , theTailStore(tail_store)
    {
        define("record_event", &KeeperRecordingService::record_event, tl::ignore_return_value());
        define("tail_get_sequences", &KeeperRecordingService::tail_get_sequences);
        define("tail_get_events", &KeeperRecordingService::tail_get_events);
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]() { delete p; });
    }

    KeeperRecordingService(KeeperRecordingService const&) = delete;

    KeeperRecordingService& operator=(KeeperRecordingService const&) = delete;

    IngestionQueue& theIngestionQueue;
    KeeperTailStore& theTailStore;
};

} // namespace chronolog

#endif
