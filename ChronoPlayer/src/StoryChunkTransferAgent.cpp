#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <ios>
#include <cstddef>
#include <thallium/serialization/stl/vector.hpp>
#include <cereal/archives/binary.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <PlaybackQueryResponse.h>
#include <StoryChunk.h>
#include <StoryChunkTransferAgent.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::StoryChunkTransferAgent::StoryChunkTransferAgent(tl::engine& tl_engine,
                                                            chronolog::ServiceId const& service_id)
    : service_engine(tl_engine)
    , receiver_service_id(service_id)
{
    std::string service_addr_string;
    receiver_service_id.get_service_as_string(service_addr_string);

    LOG_DEBUG("[StoryChunkTransferAgent] Constructor for receiver service {} , service_string {}",
              chl::to_string(receiver_service_id),
              service_addr_string);
    receiver_service_handle =
            tl::provider_handle(service_engine.lookup(service_addr_string), receiver_service_id.getProviderId());

    receiver_is_available = service_engine.define("receiver_is_available");
    receive_story_chunk = service_engine.define("receive_story_chunk");

    LOG_DEBUG("[StoryChunkTransferAgent] created agent for receiver service {}", chl::to_string(receiver_service_id));
}

chronolog::StoryChunkTransferAgent::~StoryChunkTransferAgent()
{
    receiver_is_available.deregister();
    receive_story_chunk.deregister();
    LOG_DEBUG("[StoryChunkTransferAgent] Destroying agent for receiver service {}",
              chl::to_string(receiver_service_id));
}

bool chronolog::StoryChunkTransferAgent::is_receiver_available() const
{
    bool ret_value = receiver_is_available.on(receiver_service_handle)();

    LOG_DEBUG("[StoryChunkTransferAgent] receiver_service {} is available {}",
              chl::to_string(receiver_service_id),
              ret_value);
    return ret_value;
}
int chronolog::StoryChunkTransferAgent::processStoryChunk(chronolog::StoryChunk* story_chunk)
{
    try
    {
        LOG_DEBUG(
                "[StoryChunkTransferAgent] agent for receiver {} processing a story chunk, StoryID: {}, StartTime: {}",
                chl::to_string(receiver_service_id),
                story_chunk->getStoryId(),
                story_chunk->getStartTime());
#ifdef LOGTIME
        std::chrono::high_resolution_clock::time_point start, end;
        start = std::chrono::high_resolution_clock::now();
#endif
        // Build the wire response: a flat vector of LogEvent values in the
        // chunk's natural sorted order (the chunk stores events in a map
        // keyed by (eventTime, clientId, eventIndex), so iteration is
        // already ordered).
        chronolog::PlaybackQueryResponse response;
        response.events.reserve(story_chunk->getEventCount());
        for(auto const& entry: *story_chunk) { response.events.push_back(entry.second); }

        size_t serialized_response_size;
        std::ostringstream oss(std::ios::binary);
        cereal::BinaryOutputArchive oarchive(oss);
        oarchive(response);
        std::string serialized_response = oss.str();
        serialized_response_size = serialized_response.size();

#ifdef LOGTIME
        end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[StoryChunkTransferAgent] PlaybackQueryResponse serialization took {} us",
                 std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0);
#endif
        LOG_DEBUG("[StoryChunkTransferAgent] Serialized PlaybackQueryResponse size: {} ({} events)",
                  serialized_response_size,
                  response.events.size());

        std::vector<std::pair<void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_response.data());
        segments[0].second = serialized_response_size;
        tl::bulk tl_bulk = service_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_DEBUG("[StoryChunkTransferAgent] Draining PlaybackQueryResponse size: {} ...", tl_bulk.size());

        size_t bytes_transfered = receive_story_chunk.on(receiver_service_handle)(tl_bulk);

#ifdef LOGTIME
        start = end;
        end = std::chrono::high_resolution_clock::now();
        LOG_INFO("[StoryChunkTransferAgent] PlaybackQueryResponse transfer took {} us",
                 std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0);
#endif
        LOG_DEBUG("[StoryChunkTransferAgent] PlaybackQueryResponse transfer returned with result: {}",
                  bytes_transfered);

        if(bytes_transfered == serialized_response_size)
        {
            LOG_INFO("[StoryChunkTransferAgent] Successfully transfered PlaybackQueryResponse, StoryId:{}, "
                     "StartTime:{}, events:{}",
                     story_chunk->getStoryId(),
                     story_chunk->getStartTime(),
                     response.events.size());
            return chronolog::CL_SUCCESS;
        }
    }
    catch(tl::exception const& ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Thallium exception while transferring StoryChunk: {}", ex.what());
        return (chronolog::CL_ERR_UNKNOWN);
    }
    catch(cereal::Exception const& ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Cereal exception while serializing PlaybackQueryResponse: {}", ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(std::exception const& ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Standard exception while serializing PlaybackQueryResponse: {}",
                  ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(...)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Unknown exception while transferring PlaybackQueryResponse.");
        return chronolog::CL_ERR_UNKNOWN;
    }

    LOG_ERROR("[StoryChunkTransferAgent] Failed to transfer PlaybackQueryResponse, StoryId:{},StartTime:{}",
              story_chunk->getStoryId(),
              story_chunk->getStartTime());
    return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
}
