#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>
#include "StoryChunkExtractorRDMA.h"

namespace tl = thallium;

chronolog::StoryChunkExtractorRDMA::StoryChunkExtractorRDMA(tl::engine &extraction_engine
                                                            , tl::remote_procedure &drain_to_grapher
                                                            , tl::provider_handle &service_ph): extraction_engine(
        extraction_engine), drain_to_grapher(drain_to_grapher), service_ph(service_ph)
{
    LOG_DEBUG("[StoryChunkExtractorRDMA] KeeperGrapherDrainService setup complete");
}

chronolog::StoryChunkExtractorRDMA::~StoryChunkExtractorRDMA()
{
    LOG_DEBUG("[StoryChunkExtractorRDMA] Unregistering KeeperGrapherDrainService ...");
    drain_to_grapher.deregister();
}

int chronolog::StoryChunkExtractorRDMA::processStoryChunk(StoryChunk*story_chunk)
{
    std::chrono::high_resolution_clock::time_point start, end;
    try
    {
        LOG_DEBUG("[StoryChunkExtractorRDMA] Processing a story chunk, StoryID: {}, StartTime: {} ..."
                  , story_chunk->getStoryId(), story_chunk->getStartTime());
#ifndef NDEBUG
        start = std::chrono::high_resolution_clock::now();
#endif
        size_t serialized_story_chunk_size;
        std::ostringstream oss(std::ios::binary);
        cereal::BinaryOutputArchive oarchive(oss);
        oarchive(*story_chunk);
        std::string serialized_story_chunk = oss.str();
        serialized_story_chunk_size = serialized_story_chunk.size();

#ifndef NDEBUG
        end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[StoryChunkExtractorRDMA] Serialization took {} us",
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);
#endif
        LOG_DEBUG("[StoryChunkExtractorRDMA] Serialized story chunk size: {}", serialized_story_chunk_size);

        std::vector <std::pair <void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_story_chunk.data());
        segments[0].second = serialized_story_chunk_size;
        tl::bulk tl_bulk = extraction_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_DEBUG("[StoryChunkExtractorRDMA] Draining to Grapher with story chunk size: {} ...", tl_bulk.size());
#ifndef NDEBUG
        start = std::chrono::high_resolution_clock::now();
#endif
        size_t result = drain_to_grapher.on(service_ph)(tl_bulk);
#ifndef NDEBUG
        end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[StoryChunkExtractorRDMA] Draining to Grapher took {} us",
                std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count() / 1000.0);
#endif
        LOG_DEBUG("[StoryChunkExtractorRDMA] Draining to Grapher returned with result: {}", result);

        if(result == serialized_story_chunk_size)
        {
            LOG_INFO("[StoryChunkExtractorRDMA] Successfully drained a story chunk to Grapher, StoryID: {}, "
                     "StartTime: {}", story_chunk->getStoryId(), story_chunk->getStartTime());
            return chronolog::CL_SUCCESS;
        }
        else
        {
            LOG_ERROR("[StoryChunkExtractorRDMA] Failed to drain a story chunk to Grapher, StoryID: {}, "
                      "StartTime: {}, Error Code: {}", story_chunk->getStoryId(), story_chunk->getStartTime(), result);
            return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
        }
    }
    catch(tl::exception const &ex)
    {
        LOG_ERROR("[StoryChunkExtractorRDMA] Thallium exception encountered draining story chunk to grapher.");
        LOG_ERROR("[StoryChunkExtractorRDMA] Exception: {}", ex.what());
        return (chronolog::CL_ERR_UNKNOWN);
    }
    catch(cereal::Exception const &ex)
    {
        LOG_ERROR("[StoryChunkExtractorRDMA] Cereal exception encountered serializing story chunk.");
        LOG_ERROR("[StoryChunkExtractorRDMA] Exception: {}", ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(std::exception const &ex)
    {
        LOG_ERROR("[StoryChunkExtractorRDMA] Standard exception encountered serializing story chunk.");
        LOG_ERROR("[StoryChunkExtractorRDMA] Exception: {}", ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(...)
    {
        LOG_ERROR("[StoryChunkExtractorRDMA] Unknown exception encountered draining story chunk to grapher.");
        return chronolog::CL_ERR_UNKNOWN;
    }
}
