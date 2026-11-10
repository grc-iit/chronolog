#ifndef KEEPER_RECORDING_SERVICE_H
#define KEEPER_RECORDING_SERVICE_H

#include <functional>
#include <iostream>
#include <string>
#include <cstdint>
#include <margo.h>
#include <thallium.hpp>
#include <private/RpcReply.h>
#include <private/RpcBulkOwner.h>
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
// the writer nor the WAL and the data store it calls. Empty: every durable
// write is answered CL_ERR_DURABILITY_UNAVAILABLE.
using DurableWriteFunction = std::function<int(ChronicleName const&, StoryName const&, LogEvent const&)>;

class KeeperRecordingService: public tl::provider<KeeperRecordingService>
{
public:
    // KeeperRecordingService should be created on the heap not the stack thus the constructor is private...
    static KeeperRecordingService* CreateKeeperRecordingService(tl::engine& tl_engine,
                                                                uint16_t service_provider_id,
                                                                IngestionQueue& ingestion_queue,
                                                                KeeperChunkRetentionStore& tail_store,
                                                                DurableWriteFunction durable_write = {})
    {
        auto* service = new KeeperRecordingService(tl_engine,
                                                   service_provider_id,
                                                   ingestion_queue,
                                                   tail_store,
                                                   std::move(durable_write));
        service->legacy_ = true;
        service->registerProcedures();
        tl_engine.push_finalize_callback(service, [service] { delete service; });
        return service;
    }

    static KeeperRecordingService* CreateKeeperRecordingService(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                                                                uint16_t provider,
                                                                IngestionQueue& queue,
                                                                KeeperChunkRetentionStore& store,
                                                                DurableWriteFunction durable_write = {})
    {
        if(!owner || !owner->engine || owner->provider)
            return nullptr;
        auto service = std::shared_ptr<KeeperRecordingService>(
                new KeeperRecordingService(*owner->engine, provider, queue, store, std::move(durable_write)));
        service->owner_ = owner;
        service->owned_ = true;
        owner->provider = service;
        service->registerProcedures();
        return service.get();
    }
    ~KeeperRecordingService()
    {
        LOG_DEBUG("[KeeperRecordingService] Destructor called. Cleaning up...");
        if(legacy_)
            get_engine().pop_finalize_callback(this);
    }

    void record_event(tl::request const& request, LogEvent const& log_event)
    {
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
            return; // No mutation or fabricated empty reply after quiescence.

        //  ClientId teller_id,  StoryId story_id,
        //  ChronoTick const& chrono_tick, std::string const& record)
        LOG_TRACE("[KeeperRecordingService] Recording event: storyId={}, time={}, clientId={}, index={}, record={}",
                  log_event.getStoryId(),
                  log_event.time(),
                  log_event.getClientId(),
                  log_event.index(),
                  log_event.getRecord());
        if(owner)
            owner->writeAdmitted.store(true, std::memory_order_release);
        theIngestionQueue.ingestLogEvent(log_event);
        rpc::respond(request, chronolog::CL_SUCCESS);
    }

    // A durable write: answered CL_SUCCESS only once the event is in the
    // keeper's WAL and its group's fdatasync returned, and ingested only then
    // (see KeeperDurableWriter). The handler's ULT yields while it waits, so
    // the RPC streams keep serving other requests, and its public entry keeps
    // a stop waiting until the answer is sent. Defined whether the WAL is on
    // or off; with it off the answer is CL_ERR_DURABILITY_UNAVAILABLE. After
    // quiescence the request is not answered and nothing is stored: the
    // client's attempt goes unanswered, as an append's does.
    void record_event_durable(tl::request const& request,
                              ChronicleName const& chronicle,
                              StoryName const& story,
                              LogEvent const& log_event)
    {
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
            return; // No mutation or fabricated reply after quiescence.

        LOG_TRACE("[KeeperRecordingService] Recording durable event: Chronicle={}, Story={}, storyId={}, time={}, "
                  "clientId={}, index={}",
                  chronicle,
                  story,
                  log_event.getStoryId(),
                  log_event.time(),
                  log_event.getClientId(),
                  log_event.index());
        if(!theDurableWrite)
        {
            rpc::respond(request, chronolog::CL_ERR_DURABILITY_UNAVAILABLE);
            return;
        }
        if(owner)
            owner->writeAdmitted.store(true, std::memory_order_release);
        // A client that gave up on its attempt cannot be answered; the event
        // is durable either way, and a retry finds it.
        rpc::respond(request, theDurableWrite(chronicle, story, log_event));
    }

    // Tail-read phase 1: return this keeper's most recent (up to n) event
    // sequences for the story (cheap keys only, no payload).
    void tail_get_sequences(tl::request const& request, StoryId const& story_id, uint64_t n)
    {
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
            return; // No mutation or fabricated empty reply after quiescence.

        rpc::respond(request, theTailStore.getTailSequences(story_id, (std::size_t)n));
    }

    // Tail-read phase 2: return the payloads this keeper holds for the requested
    // event sequences (the globally-selected last-N, after client-side merge).
    void tail_get_events(tl::request const& request, StoryId const& story_id, std::vector<EventSequence> const& seqs)
    {
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
            return; // No mutation or fabricated empty reply after quiescence.

        rpc::respond(request, theTailStore.getTailEvents(story_id, seqs));
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
        auto owner = owner_.lock();
        auto entry = owner ? owner->enterPublic() : std::optional<rpc::RpcBulkOwner::Entry>{};
        if(owned_ && !entry)
            return; // No mutation or fabricated empty reply after quiescence.

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
        rpc::respond(request, response);
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
    {}
    void registerProcedures()
    {
        define("record_event", &KeeperRecordingService::record_event, tl::ignore_return_value());
        define("record_event_durable", &KeeperRecordingService::record_event_durable, tl::ignore_return_value());
        define("tail_get_sequences", &KeeperRecordingService::tail_get_sequences);
        define("tail_get_events", &KeeperRecordingService::tail_get_events);
        define("story_range_fetch", &KeeperRecordingService::story_range_fetch);
    }

    KeeperRecordingService(KeeperRecordingService const&) = delete;

    KeeperRecordingService& operator=(KeeperRecordingService const&) = delete;

    bool legacy_ = false, owned_ = false;
    std::weak_ptr<rpc::RpcBulkOwner> owner_;
    IngestionQueue& theIngestionQueue;
    KeeperChunkRetentionStore& theTailStore;
    DurableWriteFunction theDurableWrite;
};

} // namespace chronolog

#endif
