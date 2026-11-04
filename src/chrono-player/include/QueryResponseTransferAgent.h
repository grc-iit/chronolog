#ifndef CHRONOLOG_QUERY_RESPONSE_AGENT_H
#define CHRONOLOG_QUERY_RESPONSE_AGENT_H
#include <PlaybackRpcOwner.h>
#include <PlaybackQueryResponse.h>
#include <private/RpcBulkSend.h>
#include <StoryChunk.h>
#include <list>
#include <map>
#include <mutex>
namespace chronolog
{
class QueryResponseAgent
{
public:
    static QueryResponseAgent* CreateQueryResponseAgent(thallium::engine& engine, ServiceId const& route)
    {
        return CreateQueryResponseAgent(PlaybackRpcOwner::Find(engine.get_margo_instance()), route);
    }
    static QueryResponseAgent* CreateQueryResponseAgent(std::shared_ptr<PlaybackRpcOwner> const& owner,
                                                        ServiceId const& route)
    {
        return owner && !owner->closing() ? new QueryResponseAgent(owner, route) : nullptr;
    }
    virtual ~QueryResponseAgent() = default;
    bool is_receiver_available() const { return false; }
    // Unreserved raw records cannot create retained payload state. Production
    // uses the owned overload under the whole-query admission below.
    int stashQueryResponseRecord(ClientQueryId const&, PlaybackQueryResponse*, bool) { return CL_ERR_RPC_CAPACITY; }
    int
    addArchivedEventsToQueryResponse(ClientQueryId const&, std::list<StoryChunk*> const&, int read_status = CL_SUCCESS);
    void drainQueryResponses() {}
    void startResponseThreads(int) {}
    void stopResponseThreads() {}
    int transferQueryResponseToClient(PlaybackQueryResponse const&);
    rpc::RpcResult<uint64_t> transferOwnedQueryResponse(std::shared_ptr<PlaybackQueryResponse const>,
                                                        rpc::RpcReservation,
                                                        rpc::BulkPermits::Permit,
                                                        rpc::RpcDeadline const& replay);
    std::shared_ptr<PlaybackQueryResponse> takeReady(ClientQueryId query);

private:
    friend class PlaybackService;
    int stashOwnedQueryResponse(ClientQueryId, std::shared_ptr<PlaybackQueryResponse> const&, bool);
    QueryResponseAgent(std::shared_ptr<PlaybackRpcOwner> owner, ServiceId route)
        : owner_(owner)
        , route_(std::move(route))
    {}
    struct Record
    {
        bool ready;
        std::shared_ptr<PlaybackQueryResponse> response;
    };
    std::weak_ptr<PlaybackRpcOwner> owner_;
    ServiceId route_;
    std::mutex mutex_;
    std::map<ClientQueryId, Record> queries_;
};
} // namespace chronolog
#endif
