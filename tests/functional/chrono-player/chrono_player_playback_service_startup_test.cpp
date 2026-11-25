// Loopback TCP requires FI_TCP_IFACE=lo when the provider default selects another NIC.
#include <PlaybackService.h>
#include <abt.h>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
namespace chronolog
{
struct PlaybackServiceTestAccess
{
    static PlaybackService* create(std::shared_ptr<PlaybackRpcOwner> const& owner,
                                   uint16_t provider,
                                   PlayerDataStore& store,
                                   ArchiveReadingRequestQueue& queue,
                                   std::function<void(PlaybackService&, bool)> observer)
    {
        return PlaybackService::CreatePlaybackService(owner, provider, store, queue, std::move(observer));
    }
};
} // namespace chronolog
namespace
{
namespace chl = chronolog;
namespace tl = thallium;
using namespace std::chrono_literals;
constexpr uint16_t provider = 56;
struct Dependencies
{
    chl::StoryChunkIngestionQueue ingestion;
    chl::PlayerDataStore store{ingestion};
    chl::ArchiveReadingRequestQueue queue;
};
struct Observation
{
    std::shared_ptr<Dependencies> dependencies = std::make_shared<Dependencies>();
    std::weak_ptr<void> providerRoot;
    bool before = false, afterFirst = false, rootedBefore = false, rootedAfterFirst = false;
};
void ensureNative()
{
    static bool ready = []
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "playback_startup_test");
        if(margo_set_environment(nullptr) || ABT_init(0, nullptr) != ABT_SUCCESS)
            throw std::runtime_error("fixture native initialization failed");
        size_t stack = 0;
        if(ABT_info_query_config(ABT_INFO_QUERY_KIND_DEFAULT_THREAD_STACKSIZE, &stack) != ABT_SUCCESS ||
           stack != 2097152)
            throw std::runtime_error("fixture differs from public Margo startup profile");
        return true;
    }();
    (void)ready;
}
class PlaybackStartup: public ::testing::TestWithParam<char const*>
{
};
TEST_P(PlaybackStartup, FirstRegisteredCallbackSurvivesLaterInitializationFailure)
{
    ensureNative();
    auto runtime = std::make_shared<chl::rpc::RpcRuntime>();
    auto owner = std::make_shared<chl::PlaybackRpcOwner>(
            std::make_unique<tl::engine>(std::string(GetParam()) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1),
            chl::RpcTimeoutPolicy{});
    owner->attach(runtime);
    owner->initializeProcedure();
    auto observed = std::make_shared<Observation>();
    std::weak_ptr<chl::PlaybackRpcOwner> weakOwner = owner;
    auto observer = [observed, weakOwner](chl::PlaybackService& service, bool afterFirst)
    {
        auto actual = weakOwner.lock();
        bool rooted = actual && actual->provider.get() == &service;
        if(!afterFirst)
        {
            observed->before = true;
            observed->rootedBefore = rooted;
            return;
        }
        observed->afterFirst = true;
        observed->rootedAfterFirst = rooted;
        observed->providerRoot = actual->provider;
        throw tl::exception("injected failure after first actual provider registration");
    };
    EXPECT_THROW(chl::PlaybackServiceTestAccess::create(owner,
                                                        provider,
                                                        observed->dependencies->store,
                                                        observed->dependencies->queue,
                                                        std::move(observer)),
                 tl::exception);
    EXPECT_TRUE(observed->before);
    EXPECT_TRUE(observed->afterFirst);
    EXPECT_TRUE(observed->rootedBefore);
    EXPECT_TRUE(observed->rootedAfterFirst);
    EXPECT_NE(nullptr, owner->provider);
    EXPECT_FALSE(observed->providerRoot.expired());
    auto* original = owner->provider.get();
    bool repeatedObserverCalled = false;
    EXPECT_THROW(chl::PlaybackServiceTestAccess::create(owner,
                                                        provider,
                                                        observed->dependencies->store,
                                                        observed->dependencies->queue,
                                                        [&](chl::PlaybackService&, bool)
                                                        { repeatedObserverCalled = true; }),
                 tl::exception);
    EXPECT_FALSE(repeatedObserverCalled);
    EXPECT_EQ(original, owner->provider.get());
    hg_id_t id = 0;
    hg_bool_t first = HG_FALSE, second = HG_FALSE;
    ASSERT_EQ(HG_SUCCESS,
              margo_provider_registered_name(owner->engine->get_margo_instance(),
                                             "playback_service_available",
                                             provider,
                                             &id,
                                             &first));
    ASSERT_EQ(HG_SUCCESS,
              margo_provider_registered_name(owner->engine->get_margo_instance(),
                                             "story_playback_request",
                                             provider,
                                             &id,
                                             &second));
    ASSERT_EQ(HG_TRUE, first);
    ASSERT_EQ(HG_FALSE, second);
    std::string address = static_cast<std::string>(owner->engine->self());
    std::fprintf(stderr,
                 "B08 first registered callback: %s root=%p before=%d after=%d\n",
                 address.c_str(),
                 owner->provider.get(),
                 observed->rootedBefore,
                 observed->rootedAfterFirst);
    std::fflush(stderr);
    tl::engine client(std::string(GetParam()) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
    {
        auto available = client.define("playback_service_available");
        auto endpoint = client.lookup(address);
        tl::provider_handle peer(endpoint, provider);
        EXPECT_EQ(1, static_cast<int>(available.on(peer).timed(1000ms)));
    }
    client.finalize();
    owner->quiesce();
    EXPECT_EQ(chl::CL_ERR_SHUTDOWN_INCOMPLETE, runtime->drain(chl::rpc::RpcDeadline::after(40ms)));
    EXPECT_NE(nullptr, owner->provider);
    owner.reset();
    EXPECT_FALSE(weakOwner.expired());
    EXPECT_FALSE(observed->providerRoot.expired());
}
INSTANTIATE_TEST_SUITE_P(Transports, PlaybackStartup, ::testing::Values("ofi+sockets", "ofi+tcp"));
} // namespace
