#ifndef GRAPHER_RECORDING_SERVICE_H
#define GRAPHER_RECORDING_SERVICE_H

#include <iostream>
#include <margo.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include "chronolog_errcode.h"
#include "KeeperIdCard.h"
#include "chronolog_types.h"
#include "ChunkIngestionQueue.h"
#include "../external_libs/cereal/include/cereal/archives/binary.hpp"

namespace tl = thallium;

namespace chronolog
{
#define MAX_BULK_MEM_SIZE (4 * 1024 * 1024)
class GrapherRecordingService: public tl::provider <GrapherRecordingService>
{
public:
    // RecordingService should be created on the heap not the stack thus the constructor is private...
    static GrapherRecordingService*
    CreateRecordingService(tl::engine &tl_engine, uint16_t service_provider_id, ChunkIngestionQueue &ingestion_queue)
    {
        return new GrapherRecordingService(tl_engine, service_provider_id, ingestion_queue);
    }

    ~GrapherRecordingService()
    {
        LOG_DEBUG("[GrapherRecordingService] Destructor called. Cleaning up...");
        get_engine().pop_finalize_callback(this);
    }

    void record_story_chunk(tl::request const &request, tl::bulk &b)
    {
        std::chrono::high_resolution_clock::time_point start, end;
        LOG_DEBUG("[GrapherRecordingService] StoryChunk recording RPC invoked, ThreadID={}", tl::thread::self_id());
        tl::endpoint ep = request.get_endpoint();
        LOG_DEBUG("[GrapherRecordingService] Endpoint obtained, ThreadID={}", tl::thread::self_id());
        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(&mem_vec[0]);
        segments[0].second = mem_vec.size();
        LOG_DEBUG("[GrapherRecordingService] Bulk memory prepared, size: {}, ThreadID={}", mem_vec.size()
                  , tl::thread::self_id());
        tl::engine tl_engine = get_engine();
        LOG_DEBUG("[GrapherRecordingService] Engine addr: {}, ThreadID={}", (void*)&tl_engine, tl::thread::self_id());
        tl::bulk local = tl_engine.expose(segments, tl::bulk_mode::write_only);
        LOG_DEBUG("[GrapherRecordingService] Bulk memory exposed, ThreadID={}", tl::thread::self_id());
        b.on(ep) >> local;
        LOG_DEBUG("[GrapherRecordingService] Received {} bytes of StoryChunk data, ThreadID={}", b.size()
                  , tl::thread::self_id());
//        for(auto i = 0; i < b.size() - 1; ++i)
//        {
//            std::cout << (char)*(char*)(&mem_vec[0]+i) << " ";
//        }
//        std::cout << std::endl;
        StoryChunk story_chunk;
#ifndef NDEBUG
        start = std::chrono::high_resolution_clock::now();
#endif
        deserializedWithCereal(&mem_vec[0], b.size() - 1, story_chunk);
#ifndef NDEBUG
        end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[GrapherRecordingService] Deserialization took {} us, ThreadID={}",
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0
                 , tl::thread::self_id());
#endif
        LOG_DEBUG("[GrapherRecordingService] StoryChunk received: StoryID: {}, StartTime: {}, ThreadID={}"
                  , story_chunk.getStoryId(), story_chunk.getStartTime(), tl::thread::self_id());
        request.respond(b.size());
        LOG_DEBUG("[GrapherRecordingService] StoryChunk recording RPC responded {}, ThreadID={}"
                  , b.size(), tl::thread::self_id());

        theIngestionQueue.ingestStoryChunk(&story_chunk);
        LOG_DEBUG("[GrapherRecordingService] Ingested a StoryChunk, StoryID: {}, StartTime: {}, ThreadID={}"
                  , story_chunk.getStoryId(), story_chunk.getStartTime(), tl::thread::self_id());
    }

private:
    GrapherRecordingService(tl::engine &tl_engine, uint16_t service_provider_id, ChunkIngestionQueue &ingestion_queue)
            : tl::provider <GrapherRecordingService>(tl_engine, service_provider_id), theIngestionQueue(ingestion_queue)
    {
        mem_vec.resize(MAX_BULK_MEM_SIZE);
        define("record_story_chunk", &GrapherRecordingService::record_story_chunk, tl::ignore_return_value());
        //set up callback for the case when the engine is being finalized while this provider is still alive
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });
    }

    void deserializedWithCereal(char *buffer, size_t size, StoryChunk &story_chunk)
    {
        try
        {
            std::stringstream ss;
            ss.write(buffer, size);
            cereal::BinaryInputArchive iarchive(ss);
            iarchive(story_chunk);
        }
        catch(cereal::Exception const &ex)
        {
            LOG_ERROR("[GrapherRecordingService] Failed to deserialize a story chunk, ThreadID={}. Cereal exception "
                      "encountered.", tl::thread::self_id());
            LOG_ERROR("[GrapherRecordingService] Exception: {}", ex.what());
        }
    }

    GrapherRecordingService(GrapherRecordingService const &) = delete;

    GrapherRecordingService &operator=(GrapherRecordingService const &) = delete;

    ChunkIngestionQueue &theIngestionQueue;

    std::vector <char> mem_vec;
};

}// namespace chronolog

#endif
