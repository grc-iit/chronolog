
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <ios>
#include <cstddef>

#include <unistd.h>
#include <cereal/archives/binary.hpp>
#include <thallium/serialization/stl/vector.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <PlaybackQueryResponse.h>
#include <StoryChunk.h>
#include <QueryResponseTransferAgent.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::QueryResponseAgent::QueryResponseAgent(tl::engine& tl_engine, chronolog::ServiceId const& service_id)
    : state(UNKNOWN)
    , service_engine(tl_engine)
    , receiver_service_id(service_id)
{
    std::string service_addr_string;
    receiver_service_id.get_service_as_string(service_addr_string);

    LOG_DEBUG("[QueryResponseAgent] Constructor for receiver service {} , service_string {}",
              chl::to_string(receiver_service_id),
              service_addr_string);
    receiver_service_handle =
            tl::provider_handle(service_engine.lookup(service_addr_string), receiver_service_id.getProviderId());

    receiver_is_available = service_engine.define("receiver_is_available");
    receive_query_response = service_engine.define("receive_query_response");

    LOG_INFO("[QueryResponseAgent] created QueryResponseAgent for receiver service {}",
             chl::to_string(receiver_service_id));
}

chronolog::QueryResponseAgent::~QueryResponseAgent()
{
    LOG_DEBUG("[QueryResponseAgent] Destroying agent for receiver service {}", chl::to_string(receiver_service_id));

    stopResponseThreads();

    {
        std::lock_guard<std::mutex> lock(query_mutex);
        for(auto query_iter = active_queries.begin(); query_iter != active_queries.end(); ++query_iter)
        {
            delete(*query_iter).second.second;
        }
        active_queries.clear();
    }

    receiver_is_available.deregister();
    receive_query_response.deregister();

    extractionThreads.clear();
    extractionStreams.clear();
}

bool chronolog::QueryResponseAgent::is_receiver_available() const
{
    bool ret_value = receiver_is_available.on(receiver_service_handle)();

    LOG_DEBUG("[QueryResponseAgent] receiver_service {} is available {}",
              chl::to_string(receiver_service_id),
              ret_value);
    return ret_value;
}

//////////

int chronolog::QueryResponseAgent::stashQueryResponseRecord(chl::ClientQueryId const& query_id,
                                                            chl::PlaybackQueryResponse* query_response,
                                                            bool ready_to_send)
{
    // stash PlaybackResponseObject into this agent's internal map of active queries
    // keyed by the ClientQueryId
    //
    // ready_send = true means the response is compelte and ready to be send to the client
    // ready_to_send = false means there's an archive portion of the response pending

    int ret_value = chl::CL_ERR_UNKNOWN;

    std::lock_guard<std::mutex> lock(query_mutex);

    auto insert_return =
            active_queries.insert(std::pair<chl::ClientQueryId, std::pair<bool, chl::PlaybackQueryResponse*>>(
                    query_id,
                    std::pair<bool, chl::PlaybackQueryResponse*>(ready_to_send, query_response)));

    if(insert_return.second)
    {
        LOG_INFO("[QueryResponseAgent] receiver for {} got request for query_id {}",
                 chl::to_string(receiver_service_id),
                 query_id);
        ret_value = chl::CL_SUCCESS;
    }

    return ret_value;
}

///////

int chronolog::QueryResponseAgent::stashStoryChunks(chl::ClientQueryId const& query_id,
                                                    std::list<chl::StoryChunk*> const& archive_chunks)
{
    std::lock_guard<std::mutex> lock(query_mutex);

    auto query_iter = active_queries.find(query_id);
    if(query_iter == active_queries.end())
    {
        return chl::CL_SUCCESS;
    }

    // add the events from the StoryChunks to the response.events vector
    chronolog::PlaybackQueryResponse* response = (*query_iter).second.second;

    for(auto& story_chunk: archive_chunks)
    {
        LOG_DEBUG(
                "[QueryResponseAgent] agent for receiver {} processing QueryId {} story chunk, story {} StartTime: {}",
                chl::to_string(receiver_service_id),
                query_id,
                story_chunk->getStoryName(),
                story_chunk->getStartTime());

        story_chunk->extractEventSeries(response->events);
    }

    //mark the query_response as ready to be sent
    (*query_iter).second.first = true;

    return chl::CL_SUCCESS;
}

////

int chronolog::QueryResponseAgent::transferQueryResponseToClient(chl::PlaybackQueryResponse const& response)
{
    chl::ClientQueryId query_id = response.query_id;

    try
    {
        size_t serialized_response_size;
        std::ostringstream oss(std::ios::binary);
        cereal::BinaryOutputArchive oarchive(oss);
        oarchive(response);
        std::string serialized_response = oss.str();
        serialized_response_size = serialized_response.size();

        LOG_DEBUG("[QueryResponseAgent] Serialized PlaybackQueryResponse size: {} ({} events)",
                  serialized_response_size,
                  response.events.size());

        std::vector<std::pair<void*, std::size_t>> segments(1);
        segments[0].first = (void*)(serialized_response.data());
        segments[0].second = serialized_response_size;
        tl::bulk tl_bulk = service_engine.expose(segments, tl::bulk_mode::read_only);
        LOG_TRACE("[QueryResponseAgent] Draining PlaybackQueryResponse QueryId {} bulk size: {} ",
                  query_id,
                  tl_bulk.size());

        size_t bytes_transfered = receive_query_response.on(receiver_service_handle)(tl_bulk);

        LOG_TRACE("[QueryResponseAgent] PlaybackQueryResponse transfer for QueryId {} returned with result: {}",
                  query_id,
                  bytes_transfered);

        if(bytes_transfered == serialized_response_size)
        {
            LOG_INFO("[QueryResponseAgent] Successfully transfered PlaybackQueryResponse for QueryId {} with {} events",
                     query_id,
                     response.events.size());
            return chronolog::CL_SUCCESS;
        }
    }
    catch(tl::exception const& ex)
    {
        LOG_ERROR("[QueryResponseAgent] Thallium exception while transferring PlaybackQueryResponse for QueryId {}: {}",
                  query_id,
                  ex.what());
        return (chronolog::CL_ERR_UNKNOWN);
    }
    catch(cereal::Exception const& ex)
    {
        LOG_ERROR("[QueryResponseAgent] Cereal exception while serializing PlaybackQueryResponse for QueryId {}: {}",
                  query_id,
                  ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(std::exception const& ex)
    {
        LOG_ERROR("[QueryResponseAgent] Standard exception while serializing PlaybackQueryResponse for QueryId {}: {}",
                  query_id,
                  ex.what());
        return chronolog::CL_ERR_UNKNOWN;
    }
    catch(...)
    {
        LOG_ERROR("[QueryResponseAgent] Unknown exception while transferring PlaybackQueryResponse.");
        return chronolog::CL_ERR_UNKNOWN;
    }

    LOG_ERROR("[QueryResponseAgent] Failed to transfer PlaybackQueryResponse for QueryId {}", query_id);

    return chronolog::CL_ERR_STORY_CHUNK_EXTRACTION;
}

////

void chronolog::QueryResponseAgent::startResponseThreads(int stream_count)
{
    state = RUNNING;

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed<tl::xstream> es = tl::xstream::create();
        extractionStreams.push_back(std::move(es));
    }

    for(int i = 0; i < stream_count; ++i)
    {
        tl::managed<tl::thread> th = extractionStreams[i % extractionStreams.size()]->make_thread(
                [p = this]() { p->drainQueryResponses(); });
        extractionThreads.push_back(std::move(th));
    }
    LOG_INFO("[QueryResponseAgent] Started query response threads for receiver  {}",
             chl::to_string(receiver_service_id));
}

//////

void chronolog::QueryResponseAgent::stopResponseThreads()
{
    if(state == SHUTTING_DOWN)
    {
        return;
    }

    state = SHUTTING_DOWN;

    // join threads & executionstreams while holding stateMutex
    for(auto& eth: extractionThreads) { eth->join(); }
    LOG_INFO("[QueryResponseAgent] Response threads for receiver {} successfully shut down.",
             chl::to_string(receiver_service_id));
    for(auto& es: extractionStreams) { es->join(); }
    LOG_INFO("[QueryResponseAgent] Streams for receiver {} have been successfully closed.",
             chl::to_string(receiver_service_id));
}

///
void chronolog::QueryResponseAgent::drainQueryResponses()
{
    LOG_DEBUG("[StoryChunkTransferAgent] Draining query responses: {}  active queries,  thread ID: {}",
              active_queries.size(),
              thallium::thread::self_id());

    while(state == RUNNING)
    {
        if(active_queries.empty())
        {
            sleep(2);
            continue;
        }

        PlaybackQueryResponse* query_response = nullptr;
        {
            std::lock_guard<std::mutex> lock(query_mutex);

            for(auto query_iter = active_queries.begin(); query_iter != active_queries.end(); ++query_iter)
            {
                if((*query_iter).second.first)
                { // query_response is ready to be sent to the client
                    query_response = (*query_iter).second.second;
                    active_queries.erase(query_iter);
                    break;
                }
            }
        }

        if(query_response != nullptr)
        {
            transferQueryResponseToClient(*query_response);
            delete query_response;
        }
    }
}
