#ifndef KEEPER_HOT_FETCH_CLIENT_H
#define KEEPER_HOT_FETCH_CLIENT_H

#include <string>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/tuple.hpp>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <HotRangeResponse.h>
#include <ServiceId.h>

namespace tl = thallium;

namespace chronolog
{

// Thin Thallium client of the keeper's story_range_fetch RPC — the player's
// on-demand hot source for replay. One instance per keeper recording service,
// created lazily by the PlaybackService and cached by endpoint.
class KeeperHotFetchClient
{
public:
    static KeeperHotFetchClient* CreateKeeperHotFetchClient(tl::engine& tl_engine, ServiceId const& keeper_service_id)
    {
        try
        {
            return new KeeperHotFetchClient(tl_engine, keeper_service_id);
        }
        catch(tl::exception const& ex)
        {
            LOG_ERROR("[KeeperHotFetchClient] Failed to create client for {} exception {}",
                      to_string(keeper_service_id),
                      ex.what());
        }
        return nullptr;
    }

    // Fetch every event the keeper retains in [start, end). On any failure an
    // empty response with hot_floor = UINT64_MAX is returned: the keeper drops
    // out of the min() and the split boundary only rises — overlap with the
    // archive is the failure-safe direction.
    HotRangeResponse fetchRange(StoryId const& story_id, uint64_t start_time, uint64_t end_time, uint64_t max_events)
    {
        try
        {
            HotRangeResponse response = story_range_fetch.on(service_ph)(story_id, start_time, end_time, max_events);
            return response;
        }
        catch(tl::exception const& ex)
        {
            LOG_WARNING("[KeeperHotFetchClient] story_range_fetch to {} failed: {}",
                        to_string(keeperServiceId),
                        ex.what());
        }
        return HotRangeResponse{};
    }

    ServiceId const& getKeeperServiceId() const { return keeperServiceId; }

    ~KeeperHotFetchClient()
    {
        story_range_fetch.deregister();
        LOG_DEBUG("[KeeperHotFetchClient] Destructor called {}", to_string(keeperServiceId));
    }

private:
    ServiceId keeperServiceId;
    tl::provider_handle service_ph;
    tl::remote_procedure story_range_fetch;

    // constructor is private to make sure thallium rpc objects are created on the heap, not stack
    KeeperHotFetchClient(tl::engine& tl_engine, ServiceId const& keeper_service_id)
        : keeperServiceId(keeper_service_id)
    {
        std::string service_addr_string;
        keeperServiceId.get_service_as_string(service_addr_string);
        service_ph = tl::provider_handle(tl_engine.lookup(service_addr_string), keeperServiceId.getProviderId());
        story_range_fetch = tl_engine.define("story_range_fetch");
    }

    KeeperHotFetchClient() = delete;
    KeeperHotFetchClient(KeeperHotFetchClient const&) = delete;
    KeeperHotFetchClient& operator=(KeeperHotFetchClient const&) = delete;
};

} // namespace chronolog

#endif // KEEPER_HOT_FETCH_CLIENT_H
