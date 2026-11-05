#ifndef PLAYBACK_QUERY_RPC_CLIENT_H
#define PLAYBACK_QUERY_RPC_CLIENT_H
#include "ClientRpcContext.h"
#include <private/RpcCallState.h>
#include <ServiceId.h>
#include <chronolog_types.h>
#include <memory>
#include <string>
namespace chronolog
{
struct PlaybackQuery;
// Lookup-free immutable route. Actual native state belongs to charged work;
// destroying a cached route never deregisters the context's shared procedures.
class PlaybackQueryRpcClient
{
public:
    static std::shared_ptr<PlaybackQueryRpcClient> CreatePlaybackQueryRpcClient(ServiceId const& service)
    {
        return std::shared_ptr<PlaybackQueryRpcClient>(new PlaybackQueryRpcClient(service));
    }
    rpc::RpcResult<uint32_t> send_story_playback_request(ClientRpcContext&,
                                                         rpc::OperationContext&,
                                                         rpc::RpcCallState&,
                                                         ServiceId const& receiver,
                                                         PlaybackQuery const&,
                                                         uint64_t attemptMs) const;

private:
    explicit PlaybackQueryRpcClient(ServiceId const& service)
        : provider_(service.getProviderId())
    {
        service.get_service_as_string(address_);
    }
    std::string address_;
    uint16_t const provider_;
};
} // namespace chronolog
#endif
