#ifndef STORY_CHUNK_CONSUMER_SERVICE_H
#define STORY_CHUNK_CONSUMER_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <cereal/archives/binary.hpp>

#include <chronolog_errcode.h>
#include <ServiceId.h>
#include <chronolog_types.h>

#include "StoryChunkIngestionQueue.h"

namespace tl = thallium;

namespace chronolog
{
class StoryChunkConsumerService: public tl::provider<StoryChunkConsumerService>
{
public:
    // Service should be created on the heap not the stack thus the constructor is private...
    static StoryChunkConsumerService* CreateChunkConsumerService(tl::engine& tl_engine,
                                                                 uint16_t service_provider_id,
                                                                 StoryChunkIngestionQueue& ingestion_queue)
    {
        return new StoryChunkConsumerService(tl_engine, service_provider_id, ingestion_queue);
    }

    ~StoryChunkConsumerService()
    {
        LOG_DEBUG("[StoryChunkConsumerService] Destructor called. Cleaning up...");
        get_engine().pop_finalize_callback(this);
    }

    void receiver_available(tl::request const& request) { request.respond(true); }

    void receive_story_chunk(tl::request const& request, tl::bulk& b)
    {
        try
        {
            std::vector<char> mem_vec(b.size());
            std::chrono::high_resolution_clock::time_point start, end;
            LOG_DEBUG("[StoryChunkConsumerService] StoryChunk recording RPC invoked, ThreadID={}",
                      tl::thread::self_id());
            tl::endpoint ep = request.get_endpoint();
            LOG_DEBUG("[StoryChunkConsumerService] Endpoint obtained, ThreadID={}", tl::thread::self_id());
            std::vector<std::pair<void*, std::size_t>> segments(1);
            segments[0].first = (void*)(&mem_vec[0]);
            segments[0].second = mem_vec.size();
            LOG_DEBUG("[StoryChunkConsumerService] Bulk memory prepared, size: {}, ThreadID={}",
                      mem_vec.size(),
                      tl::thread::self_id());
            tl::engine tl_engine = get_engine();
            LOG_DEBUG("[StoryChunkConsumerService] Engine addr: {}, ThreadID={}",
                      (void*)&tl_engine,
                      tl::thread::self_id());
            tl::bulk local = tl_engine.expose(segments, tl::bulk_mode::write_only);
            LOG_DEBUG("[StoryChunkConsumerService] Bulk memory exposed, ThreadID={}", tl::thread::self_id());
            b.on(ep) >> local;
            LOG_DEBUG("[StoryChunkConsumerService] Received {} bytes of StoryChunk data, ThreadID={}",
                      b.size(),
                      tl::thread::self_id());

            StoryChunk* story_chunk = new StoryChunk();
#ifndef NDEBUG
            start = std::chrono::high_resolution_clock::now();
#endif
            int ret = deserializedWithCereal(&mem_vec[0], b.size(), *story_chunk);
            if(ret != chronolog::CL_SUCCESS)
            {
                LOG_ERROR("[StoryChunkConsumerService] Failed to deserialize a story chunk, ThreadID={}",
                          tl::thread::self_id());
                delete story_chunk;
                ret = 10000000 + tl::thread::self_id(); // arbitrary error code encoded with thread id
                LOG_ERROR("[StoryChunkConsumerService] Discarding the story chunk, responding {} to Keeper", ret);
                request.respond(ret);
                return;
            }
#ifndef NDEBUG
            end = std::chrono::high_resolution_clock::now();
            LOG_INFO("[StoryChunkConsumerService] Deserialization took {} us, ThreadID={}",
                     std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0,
                     tl::thread::self_id());
#endif
            LOG_DEBUG("[StoryChunkConsumerService] StoryChunk received: StoryId {} StartTime {} eventCount {} "
                      "ThreadID={}",
                      story_chunk->getStoryId(),
                      story_chunk->getStartTime(),
                      story_chunk->getEventCount(),
                      tl::thread::self_id());

            request.respond(b.size());
            LOG_DEBUG("[StoryChunkConsumerService] StoryChunk recording RPC responded {}, ThreadID={}",
                      b.size(),
                      tl::thread::self_id());

            theIngestionQueue.ingestStoryChunk(story_chunk);
        }
        catch(std::bad_alloc const& ex)
        {
            LOG_ERROR("[StoryChunkConsumerService] Failed to allocate memory for StoryChunk data, ThreadID={}",
                      tl::thread::self_id());
            request.respond(20000000 + tl::thread::self_id());
            return;
        }
    }

private:
    StoryChunkConsumerService(tl::engine& tl_engine,
                              uint16_t service_provider_id,
                              StoryChunkIngestionQueue& ingestion_queue)
        : tl::provider<StoryChunkConsumerService>(tl_engine, service_provider_id)
        , theIngestionQueue(ingestion_queue)
    {

        define("receiver_available", &StoryChunkConsumerService::receiver_available);
        define("receive_story_chunk", &StoryChunkConsumerService::receive_story_chunk);
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]() { delete p; });
    }

    int deserializedWithCereal(char* buffer, size_t size, StoryChunk& story_chunk)
    {
        std::stringstream ss(std::ios::binary | std::ios::in | std::ios::out);
        try
        {
            ss.write(buffer, size);
            cereal::BinaryInputArchive iarchive(ss);
            iarchive(story_chunk);
            return chronolog::CL_SUCCESS;
        }
        catch(cereal::Exception const& ex)
        {
            LOG_ERROR("[StoryChunkConsumerService] Failed to deserialize a story chunk, size={}, ThreadID={}. "
                      "Cereal exception encountered.",
                      ss.str().size(),
                      tl::thread::self_id());
            LOG_ERROR("[StoryChunkConsumerService] Exception: {}", ex.what());
            return chronolog::CL_ERR_UNKNOWN;
        }
        catch(std::exception const& ex)
        {
            LOG_ERROR("[StoryChunkConsumerService] Failed to deserialize a story chunk, size={}, ThreadID={}. "
                      "std::exception encountered.",
                      ss.str().size(),
                      tl::thread::self_id());
            LOG_ERROR("[StoryChunkConsumerService] Exception: {}", ex.what());
            return chronolog::CL_ERR_UNKNOWN;
        }
        catch(...)
        {
            LOG_ERROR("[StoryChunkConsumerService] Failed to deserialize a story chunk, ThreadID={}. Unknown exception "
                      "encountered.",
                      tl::thread::self_id());
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    StoryChunkConsumerService(StoryChunkConsumerService const&) = delete;

    StoryChunkConsumerService& operator=(StoryChunkConsumerService const&) = delete;

    StoryChunkIngestionQueue& theIngestionQueue;
};

} // namespace chronolog

#endif
