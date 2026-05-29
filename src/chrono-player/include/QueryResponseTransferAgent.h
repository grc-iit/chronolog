#ifndef CHRONOLOG_QUERY_RESPONSE_AGENT_H
#define CHRONOLOG_QUERY_RESPONSE_AGENT_H

#include <string>
#include <cstdint>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_client.h>
#include <PlaybackQueryResponse.h>
#include <ServiceId.h>

namespace chronolog
{

class StoryChunk;

class QueryResponseAgent
{

    enum AgentState
    {
        UNKNOWN = 0,
        RUNNING = 1,      //  active threads
        SHUTTING_DOWN = 2 // shutting down threads
    };

public:
    static QueryResponseAgent* CreateQueryResponseAgent(thallium::engine& tl_engine,
                                                        ServiceId const& receiver_service_id)
    {
        QueryResponseAgent* queryResponseAgent = nullptr;
        try
        {
            queryResponseAgent = new QueryResponseAgent(tl_engine, receiver_service_id);
        }
        catch(thallium::exception const& ex)
        {
            LOG_ERROR("[QueryResponseAgent] Failed to create QueryResponseAgent for service {}",
                      to_string(receiver_service_id));
        }
        return queryResponseAgent;
    }


    virtual ~QueryResponseAgent();

    bool is_receiver_available() const;

    int stashQueryResponseRecord(ClientQueryId const&, PlaybackQueryResponse*, bool);

    int stashStoryChunks(ClientQueryId const& query_id, std::list<StoryChunk*> const&);

    void drainQueryResponses();

    int transferQueryResponseToClient(PlaybackQueryResponse const&);

    void startResponseThreads(int);

    void stopResponseThreads();

private:
    // constructor is private to make sure thalium rpc objects are created on the heap, not stack
    QueryResponseAgent(thallium::engine& tl_engine, ServiceId const& receiver_service_id);

    // transfer agents can't be copied
    QueryResponseAgent(QueryResponseAgent const&) = delete;
    QueryResponseAgent& operator=(QueryResponseAgent const&) = delete;

    std::atomic<AgentState> state;

    thallium::engine& service_engine;                  // local tl::engine
    ServiceId receiver_service_id;                     // remote receiver service ServiceId
    thallium::provider_handle receiver_service_handle; // tl::provider_handle for remote receiver service
    thallium::remote_procedure receiver_is_available;
    thallium::remote_procedure receive_query_response;
    std::vector<thallium::managed<thallium::xstream>> extractionStreams;
    std::vector<thallium::managed<thallium::thread>> extractionThreads;

    //map of active client queries the receiver service is waiting on
    std::mutex query_mutex;
    std::map<ClientQueryId, std::pair<bool, PlaybackQueryResponse*>> active_queries;
};


} // namespace chronolog


#endif //CHRONOLOG_QUERY_RESPONSE_AGENT_H
