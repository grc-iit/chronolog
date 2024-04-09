#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>
#include "StoryChunkExtractorRDMA.h"

namespace tl = thallium;

#define MAX_BULK_MEM_SIZE (2 * 1024 * 1024)

chronolog::StoryChunkExtractorRDMA::StoryChunkExtractorRDMA(tl::engine &extraction_engine
                                                            , const ChronoLog::ConfigurationManager &confManager)
        : extraction_engine(extraction_engine), confManager(confManager)
{
    uint64_t tid = gettid();
    std::string KEEPER_GRAPHER_NA_STRING =
            confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
            confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.IP + ":" +
            std::to_string(confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.BASE_PORT);
    drain_to_grapher = extraction_engine.define("bulk_put");
    LOG_DEBUG("[StoryChunkExtractorRDMA] Looking up RPC at: {} ...", KEEPER_GRAPHER_NA_STRING);
    service_ph = extraction_engine.lookup(KEEPER_GRAPHER_NA_STRING);
    if(service_ph.is_null())
    {
        LOG_ERROR("[StoryChunkExtractorRDMA] Failed to lookup Grapher service provider handle");
        throw std::runtime_error("Failed to lookup Grapher service provider handle");
    }
    LOG_DEBUG("[StoryChunkExtractorRDMA] KeeperGrapherDrainService setup complete");
}

chronolog::StoryChunkExtractorRDMA::~StoryChunkExtractorRDMA()
{
    uint64_t tid = gettid();
    LOG_DEBUG("[StoryChunkExtractorRDMA] Deregistering KeeperGrapherDrainService ...");
    drain_to_grapher.deregister();
}

int chronolog::StoryChunkExtractorRDMA::processStoryChunk(StoryChunk*story_chunk)
{
    uint64_t tid = gettid();
    std::chrono::high_resolution_clock::time_point start, end;
    try
    {
//        std::string KEEPER_GRAPHER_NA_STRING =
//                confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.PROTO_CONF + "://" +
//                confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.IP + ":" +
//                std::to_string(confManager.KEEPER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF.RPC_CONF.BASE_PORT);
//        drain_to_grapher = extraction_engine.define("rdma_put");
//        LOG_DEBUG("[StoryChunkExtractorRDMA] Looking up RPC at: {} ...", KEEPER_GRAPHER_NA_STRING);
//        service_ph = extraction_engine.lookup(KEEPER_GRAPHER_NA_STRING);

        LOG_DEBUG("[StoryChunkExtractorRDMA] Processing a story chunk, StoryID: {}, StartTime: {} ..."
                  , story_chunk->getStoryId(), story_chunk->getStartTime());
        std::vector <std::pair <void*, std::size_t>> segments(1);
        start = std::chrono::high_resolution_clock::now();
        char*serialized_buf = new char[MAX_BULK_MEM_SIZE];
        size_t serialized_story_chunk_size = serializeStoryChunkWithCereal(story_chunk, serialized_buf);
        LOG_DEBUG("[StoryChunkExtractorRDMA] Serialized story chunk size: {}", serialized_story_chunk_size);
        end = std::chrono::high_resolution_clock::now();
        segments[0].first = (void*)(serialized_buf);
        segments[0].second = serialized_story_chunk_size + 1;
        tl::bulk tl_bulk = extraction_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_DEBUG("[StoryChunkExtractorRDMA] Calling RPC on Grapher with story chunk size: {} ...", tl_bulk.size());
        int result = drain_to_grapher.on(service_ph)(tl_bulk);
        LOG_DEBUG("[StoryChunkExtractorRDMA] RPC call on Grapher returned with result: {}", result);
        LOG_INFO("[StoryChunkExtractorRDMA] RPC call on Grapher took {} us",
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);

        if(result == serialized_story_chunk_size + 1)
        {
            LOG_INFO("[StoryChunkExtractorRDMA] Successfully drained a story chunk to Grapher, StoryID: {}, "
                     "StartTime: {}", story_chunk->getStoryId(), story_chunk->getStartTime());
        }
        else
        {
            LOG_ERROR("[StoryChunkExtractorRDMA] Failed to drain a story chunk to Grapher, Error Code: {}", result);
        }
        delete[] serialized_buf;
        return result;
    }
    catch(tl::exception const &)
    {
        LOG_ERROR("[StoryChunkExtractorRDMA] Failed to drain a story chunk to Grapher. "
                  "Thallium exception encountered.");
    }
    return (chronolog::CL_ERR_UNKNOWN);
}

size_t
chronolog::StoryChunkExtractorRDMA::serializeStoryChunkWithCereal(chronolog::StoryChunk*story_chunk, char*mem_ptr)
{
    std::stringstream ss;
    {
        cereal::BinaryOutputArchive oarchive(ss);
        oarchive(*story_chunk);
    }
    std::string serialized_str = ss.str();
    size_t serialized_size = serialized_str.size();
    memcpy(mem_ptr, serialized_str.c_str(), serialized_size);
    return serialized_size;
}
