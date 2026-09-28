#ifndef KEEPER_HOT_FETCH_CLIENT_H
#define KEEPER_HOT_FETCH_CLIENT_H

#include <chrono>
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
    // same bound as the client's tail-read RPCs
    static constexpr std::chrono::milliseconds kDefaultFetchDeadline{5000};

    static KeeperHotFetchClient*
    CreateKeeperHotFetchClient(tl::engine& tl_engine,
                               ServiceId const& keeper_service_id,
                               std::chrono::milliseconds fetch_deadline = kDefaultFetchDeadline)
    {
        try
        {
            return new KeeperHotFetchClient(tl_engine, keeper_service_id, fetch_deadline);
        }
        catch(tl::exception const& ex)
        {
            LOG_ERROR("[KeeperHotFetchClient] Failed to create client for {} exception {}",
                      to_string(keeper_service_id),
                      ex.what());
        }
        return nullptr;
    }

    // Fetch every event the keeper retains in [start, end), waiting at most the
    // fetch deadline. On any failure -- an RPC error or a keeper that does not
    // answer in time -- an empty response is returned (no events, known_W = 0,
    // hot_floor = UINT64_MAX) with answered = false, which leaves the split
    // boundary to the keepers that did answer and tells the player its reply
    // may be short of what this keeper holds.
    HotRangeResponse fetchRange(StoryId const& story_id, uint64_t start_time, uint64_t end_time, uint64_t max_events)
    {
        try
        {
            HotRangeResponse response =
                    story_range_fetch.on(service_ph).timed(fetchDeadline, story_id, start_time, end_time, max_events);
            return response;
        }
        catch(tl::timeout const&)
        {
            // tl::timeout is not a tl::exception
            LOG_WARNING("[KeeperHotFetchClient] story_range_fetch to {} timed out after {} ms",
                        to_string(keeperServiceId),
                        fetchDeadline.count());
        }
        catch(tl::exception const& ex)
        {
            LOG_WARNING("[KeeperHotFetchClient] story_range_fetch to {} failed: {}",
                        to_string(keeperServiceId),
                        ex.what());
        }
        return unansweredResponse();
    }

    // Issue the same fetch without waiting for it. The player asks every keeper
    // of a story, so issuing them all before waiting on any keeps one silent
    // keeper from adding its deadline to each of the others. The handle is
    // waited on with waitForRange.
    tl::async_response
    fetchRangeAsync(StoryId const& story_id, uint64_t start_time, uint64_t end_time, uint64_t max_events)
    {
        return story_range_fetch.on(service_ph).timed_async(fetchDeadline, story_id, start_time, end_time, max_events);
    }

    // Wait on a handle from fetchRangeAsync, with the same failure contract as
    // fetchRange: any failure is an empty response with answered = false.
    HotRangeResponse waitForRange(tl::async_response& pending)
    {
        try
        {
            HotRangeResponse response = pending.wait();
            return response;
        }
        catch(tl::timeout const&)
        {
            LOG_WARNING("[KeeperHotFetchClient] story_range_fetch to {} timed out after {} ms",
                        to_string(keeperServiceId),
                        fetchDeadline.count());
        }
        catch(tl::exception const& ex)
        {
            LOG_WARNING("[KeeperHotFetchClient] story_range_fetch to {} failed: {}",
                        to_string(keeperServiceId),
                        ex.what());
        }
        return unansweredResponse();
    }

    ServiceId const& getKeeperServiceId() const { return keeperServiceId; }

    static HotRangeResponse unansweredResponse()
    {
        HotRangeResponse unanswered;
        unanswered.answered = false;
        return unanswered;
    }

    // Leaves story_range_fetch registered: the engine registers an RPC once per
    // name, so every client of every keeper shares that registration, and
    // deregistering it here would cut off the others. The player deletes a
    // client whenever two replays race to create one for the same keeper.
    ~KeeperHotFetchClient() { LOG_DEBUG("[KeeperHotFetchClient] Destructor called {}", to_string(keeperServiceId)); }

private:
    ServiceId keeperServiceId;
    std::chrono::milliseconds fetchDeadline;
    tl::provider_handle service_ph;
    tl::remote_procedure story_range_fetch;

    // constructor is private to make sure thallium rpc objects are created on the heap, not stack
    KeeperHotFetchClient(tl::engine& tl_engine,
                         ServiceId const& keeper_service_id,
                         std::chrono::milliseconds fetch_deadline)
        : keeperServiceId(keeper_service_id)
        , fetchDeadline(fetch_deadline)
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
