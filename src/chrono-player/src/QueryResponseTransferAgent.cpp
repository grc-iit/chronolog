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
#include <QueryResponseTransferAgent.h>

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
    receive_query_response = service_engine.define("receive_query_response");

    LOG_DEBUG("[StoryChunkTransferAgent] created agent for receiver service {}", chl::to_string(receiver_service_id));
}

chronolog::StoryChunkTransferAgent::~StoryChunkTransferAgent()
{
    receiver_is_available.deregister();
    receive_query_response.deregister();
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

//////////

chl::PlaybackQueryResponse * chronolog::StoryChunkTransferAgent::createQueryResponse(chl::ClientQueryId const& query_id)
{
//TODO: create the PlaybackQueryResponse object and stash it this agent's internal queue or map keyed by clientQueryId
// note that particualr QueryResponseAgent talkes to one client so can jsut use client provided query id
//
    chl::PlaybackQueryResponse * new_query_response =nullptr;

    //it's not actually the queryresponse object pointer than needs to be exposed back to the PlaybackService 
    //but the reference to the event series vector
    // the bool ready to send is flipped when the archive resposne is received
    active_queries[query_id] = std::pair< chl::PlaybackQueryResponse, bool>(chl::PlaybackQueryResponse(query_id), false);


    return new_query_response;
}

///////

int chronolog::StoryChunkTransferAgent::stashStoryChunks(chl::ClientQueryId const& query_id, std::list<chl::StoryChunk*>const& archive_chunks)
{
    try
    {
	auto query_iter = active_queries.find(query_id);
	if(query_iter == active_queries.end())
	{ return chl::CL_SUCCESS; }

	// add the events from the StoryChunks to the response.events vector
        chronolog::PlaybackQueryResponse & response = (*query_iter).second.first;
      
        for( auto& story_chunk: archive_chunks)
        {	       
            LOG_DEBUG(
                "[StoryChunkTransferAgent] agent for receiver {} processing QueryId {} story chunk, story {} StartTime: {}",
                chl::to_string(receiver_service_id),
		query_id, story_chunk->getStoryName(), story_chunk->getStartTime());

            story_chunk->extractEventSeries(response.events);
        }

	(*query_iter).second.second = true;

//TODO: the rest of this function is actual transfer and should be separate function
	       // executed by the transfer thread

        size_t serialized_response_size;
        std::ostringstream oss(std::ios::binary);
        cereal::BinaryOutputArchive oarchive(oss);
        oarchive(response);
        std::string serialized_response = oss.str();
        serialized_response_size = serialized_response.size();

        LOG_DEBUG("[StoryChunkTransferAgent] Serialized PlaybackQueryResponse size: {} ({} events)",
                  serialized_response_size,
                  response.events.size());

        std::vector<std::pair<void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_response.data());
        segments[0].second = serialized_response_size;
        tl::bulk tl_bulk = service_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_TRACE("[StoryChunkTransferAgent] Draining PlaybackQueryResponse QueryId {} bulk size: {} ", query_id, tl_bulk.size());

        size_t bytes_transfered = receive_query_response.on(receiver_service_handle)(tl_bulk);

        LOG_TRACE("[StoryChunkTransferAgent] PlaybackQueryResponse transfer for QueryId {} returned with result: {}",
                  query_id, bytes_transfered);

        if(bytes_transfered == serialized_response_size)
        {
            LOG_INFO("[StoryChunkTransferAgent] Successfully transfered PlaybackQueryResponse for QueryId {} with {} events", 
                     query_id, response.events.size());
            return chronolog::CL_SUCCESS;
        }
    }
    catch(tl::exception const& ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Thallium exception while transferring PlaybackQueryResponse for QueryId {}: {}",
            query_id, ex.what());
        return (chronolog::CL_ERR_UNKNOWN);
    }
    catch(cereal::Exception const& ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Cereal exception while serializing PlaybackQueryResponse for QueryId {}: {}", 
	    query_id,ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(std::exception const& ex)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Standard exception while serializing PlaybackQueryResponse for QueryId {}: {}", 
	    query_id, ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(...)
    {
        LOG_ERROR("[StoryChunkTransferAgent] Unknown exception while transferring PlaybackQueryResponse.");
        return chronolog::CL_ERR_UNKNOWN;
    }

    LOG_ERROR("[StoryChunkTransferAgent] Failed to transfer PlaybackQueryResponse for QueryId {}", query_id);

    return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
}
