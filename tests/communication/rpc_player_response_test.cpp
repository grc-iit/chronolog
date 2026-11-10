#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <PlaybackService.h>
#include <QueryResponseTransferAgent.h>
#include <private/RpcBulkSerialization.h>
#include <chrono_monitor.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <iostream>
using namespace std::chrono_literals;
namespace tl = thallium;
namespace chl = chronolog;
using Clock = std::chrono::steady_clock;
namespace
{
template <class T>
auto hotCount(T const& owner, int) -> decltype(owner.hotPermits.used())
{
    return owner.hotPermits.used();
}
template <class T>
unsigned hotCount(T const&, long)
{
    return 0;
}
template <class T>
auto root(T& owner, std::shared_ptr<void> state, int) -> decltype(owner.dependencies = state, void())
{
    owner.dependencies = std::move(state);
}
template <class T>
void root(T&, std::shared_ptr<void> state, long)
{
    static std::shared_ptr<void> retained;
    retained = std::move(state);
}
} // namespace
int main(int argc, char** argv)
{
    if(argc != 2)
        return 2;
    chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "player_response");
    margo_set_environment(nullptr);
    ABT_init(0, nullptr);
    struct State
    {
        chl::StoryChunkIngestionQueue ingestion;
        chl::PlayerDataStore store{ingestion};
        chl::ArchiveReadingRequestQueue archives;
        std::atomic<bool> release{false}, stop{false};
        std::atomic<unsigned> fetches{0}, responses{0};
        std::shared_ptr<tl::engine> keepers, client;
    };
    auto state = std::make_shared<State>();
    std::string protocol = argv[1];
    state->keepers = std::make_shared<tl::engine>(protocol + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    state->client = std::make_shared<tl::engine>(protocol + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    auto native = std::make_unique<tl::engine>(protocol + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 100;
    policy.transfer_ms = 200;
    policy.replay_ms = 1500;
    auto owner = std::make_shared<chl::PlaybackRpcOwner>(std::move(native), policy);
    auto runtime = std::make_shared<chl::rpc::RpcRuntime>(6);
    owner->attach(runtime);
    owner->initializeProcedure();
    root(*owner, state, 0);
    auto service = chl::PlaybackService::CreatePlaybackService(owner, 25, state->store, state->archives);
    auto serviceId = [&](tl::engine& engine, uint16_t provider)
    {
        std::string self = engine.self();
        return chl::ServiceId(protocol,
                              "127.0.0.1",
                              static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1))),
                              provider);
    };
    auto keeper = state->keepers->define(
            "story_range_fetch",
            [state](tl::request const& request, chl::StoryId, uint64_t, uint64_t, uint64_t)
            {
                ++state->fetches;
                while(!state->release) margo_thread_sleep(state->keepers->get_margo_instance(), 1);
                chl::HotRangeResponse reply;
                request.respond(reply);
            },
            uint16_t{31});
    auto keeper2 = state->keepers->define(
            "story_range_fetch",
            [state](tl::request const& request, chl::StoryId, uint64_t, uint64_t, uint64_t)
            {
                ++state->fetches;
                while(!state->release) margo_thread_sleep(state->keepers->get_margo_instance(), 1);
                chl::HotRangeResponse reply;
                request.respond(reply);
            },
            uint16_t{32});
    state->store.setStoryKeepers(19, {serviceId(*state->keepers, 31), serviceId(*state->keepers, 32)});
    auto receive = state->client->define(
            "receive_query_response",
            [state](tl::request const& request, tl::bulk& source)
            {
                std::vector<char> bytes(source.size());
                auto target = state->client->expose({{bytes.data(), bytes.size()}}, tl::bulk_mode::write_only);
                source.on(request.get_endpoint()) >> target;
                chl::PlaybackQueryResponse response;
                if(!chl::rpc::deserializeBulk(bytes.data(), bytes.size(), response))
                    return;
                if(!response.complete && response.query_id > 0)
                    ++state->responses;
                request.respond(bytes.size());
            },
            uint16_t{41});
    auto archiveStream = tl::xstream::create();
    auto archives = archiveStream->make_thread(
            [state]
            {
                while(!state->stop)
                {
                    auto* request = state->archives.popReadingRequest();
                    if(request)
                    {
                        std::list<chl::StoryChunk*> empty;
                        request->queryResponseAgent->addArchivedEventsToQueryResponse(request->queryId, empty);
                        delete request;
                    }
                    else
                        margo_thread_sleep(state->client->get_margo_instance(), 1);
                }
            });
    auto remote = state->client->define("story_playback_request");
    auto destination = serviceId(*owner->engine, 25);
    std::string address;
    destination.get_service_as_string(address);
    auto peer = tl::provider_handle(state->client->lookup(address), 25);
    bool early = true, ids = true;
    for(uint32_t query = 1; query <= 3; ++query)
    {
        auto before = Clock::now();
        uint32_t ack = remote.on(peer).timed(600ms,
                                             serviceId(*state->client, 41),
                                             query,
                                             chl::StoryId{19},
                                             std::string("chron"),
                                             std::string("story"),
                                             uint64_t{100},
                                             uint64_t{200});
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - before).count();
        if(query == 1)
            early = ms < 70;
        ids = ids && ack == query;
        auto until = Clock::now() + 1s;
        while(state->responses < query && Clock::now() < until) std::this_thread::sleep_for(1ms);
        std::cout << "query=" << query << " ack=" << ack << " ack_ms=" << ms << " replies=" << state->responses
                  << " hot_calls=" << state->fetches << " hot_permits=" << hotCount(*owner, 0) << std::endl;
    }
    // Both keepers hold every fetch. Each fetch's timed forward is cancelled
    // and returns, so it gives back its slot and hot permit, and every query
    // fetches from both keepers again.
    bool bounded = early && ids && state->responses == 3 && state->fetches == 6 && hotCount(*owner, 0) == 0;
    state->stop = true;
    archives->join();
    archiveStream->join();
    state->release = true;
    if(!bounded)
        return 1;
    std::cout << "PASS every query answers and unanswered hot children give their slots back" << std::endl;
    return 0;
}
