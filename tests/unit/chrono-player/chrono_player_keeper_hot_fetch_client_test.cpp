// Unit tests for KeeperHotFetchClient, the player's client of a keeper's
// story_range_fetch. A replay fans out over every keeper of the story and
// splits at the minimum hot floor, so a keeper that never answers must cost
// at most the fetch deadline and then drop out of the minimum; it must not
// hold the replay. Runs over ofi+sockets and ofi+tcp on 127.0.0.1 against the keeper's
// real recording service and against a keeper that holds every request.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <abt.h>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <HotRangeResponse.h>
#include <ServiceId.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionQueue.h>

#include <IngestionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperRecordingService.h>

#include <KeeperHotFetchClient.h>
#include <PlaybackService.h>

namespace chl = chronolog;
namespace tl = thallium;
namespace chronolog
{
struct KeeperHotFetchClientTestAccess
{
    static rpc::RpcDeadline
    deadline(rpc::RpcDeadline const& replay, rpc::RpcDeadline::Clock::time_point started, std::chrono::milliseconds cap)
    {
        return KeeperHotFetchClient::fanoutDeadline(replay, started, cap);
    }
};
} // namespace chronolog
TEST(HotFetchPhaseDeadline, DoesNotExtendAnEarlierReplayExpiry)
{
    auto started = chl::rpc::RpcDeadline::Clock::now();
    auto replay = chl::rpc::RpcDeadline::fromStart(started, std::chrono::milliseconds(100));
    auto phase = chl::KeeperHotFetchClientTestAccess::deadline(replay,
                                                               started + std::chrono::milliseconds(50),
                                                               std::chrono::milliseconds(300));
    EXPECT_EQ(replay.expiresAt(), phase.expiresAt());
}

namespace
{
constexpr uint16_t kLiveKeeperProvider = 21;
constexpr uint16_t kHungKeeperProvider = 22;
constexpr chl::StoryId kStory = 5;
constexpr std::chrono::milliseconds kDeadline{300};
constexpr std::chrono::seconds kHang{3};

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "keeper_hot_fetch_client_test_logger");
        done = true;
    }
}

// Initialized once and never finalized, so margo leaves Argobots alone at
// engine finalize.
void ensureArgobots()
{
    static bool done = false;
    if(!done)
    {
        // Apply the same public Margo pre-ABT profile as the production
        // cold initialization path, before ABT can freeze its defaults.
        if(margo_set_environment(nullptr) != 0 || ABT_init(0, nullptr) != ABT_SUCCESS)
            throw std::runtime_error("fixture native initialization failed");
        size_t stackSize = 0;
        int query = ABT_info_query_config(ABT_INFO_QUERY_KIND_DEFAULT_THREAD_STACKSIZE, &stackSize);
        std::fprintf(stderr,
                     "B08 fixture ABT stack=%zu query=%d env=%s cache=%s\n",
                     stackSize,
                     query,
                     std::getenv("ABT_THREAD_STACKSIZE"),
                     std::getenv("ABT_MEM_MAX_NUM_STACKS"));
        if(query != ABT_SUCCESS || stackSize != 2097152)
            throw std::runtime_error("fixture native stack profile differs from Margo");
        done = true;
    }
}

// A keeper that holds every story_range_fetch for kHang (or until released)
// before answering with a floor that would win the minimum if it were used.
class HungKeeper: public tl::provider<HungKeeper>
{
public:
    HungKeeper(tl::engine& engine, uint16_t provider_id)
        : tl::provider<HungKeeper>(engine, provider_id)
    {
        define("story_range_fetch", &HungKeeper::story_range_fetch);
    }

    void story_range_fetch(tl::request const& request, chl::StoryId const&, uint64_t, uint64_t, uint64_t)
    {
        active++;
        auto const until = std::chrono::steady_clock::now() + kHang;
        while(!released && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        chl::HotRangeResponse response;
        response.hot_floor = 0;
        request.respond(response);
        replies++;
        active--;
    }

    std::atomic<bool> released{false};
    std::atomic<int> active{0}, replies{0};
};

class KeeperHotFetch: public ::testing::TestWithParam<char const*>
{
protected:
    void SetUp() override
    {
        ensureLogger();
        ensureArgobots();
        keeperEngine =
                std::make_unique<tl::engine>(std::string(GetParam()) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 2);
        playerEngine =
                std::make_unique<tl::engine>(std::string(GetParam()) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);

        playerOwner = std::make_shared<chl::PlaybackRpcOwner>(std::move(playerEngine), chl::RpcTimeoutPolicy{});
        runtime = std::make_shared<chl::rpc::RpcRuntime>(runtimeCapacity);
        playerOwner->attach(runtime);
        playerOwner->initializeProcedure();

        // the live keeper retains one chunk of the story, [100, 200)
        auto* chunk = new chl::StoryChunk("chron", "story", kStory, 100, 200);
        chunk->insertEvent(chl::LogEvent(kStory, 150, 1, 0, "event"));
        retentionStore.ingestSealedChunk(kStory, chunk);
        chl::KeeperRecordingService::CreateKeeperRecordingService(
                *keeperEngine,
                kLiveKeeperProvider,
                ingestionQueue,
                retentionStore,
                // a keeper without a WAL; these tests make no
                // durable writes
                [](chl::ChronicleName const&, chl::StoryName const&, chl::LogEvent const&)
                { return chl::CL_ERR_DURABILITY_UNAVAILABLE; });
        hungKeeper = std::make_unique<HungKeeper>(*keeperEngine, kHungKeeperProvider);
    }

    void TearDown() override
    {
        // let a held request answer while the player engine is still up
        hungKeeper->released = true;
        while(hungKeeper->active > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        liveClient.reset();
        hungClient.reset();
        // Unknown native calls and incoming settlement stay retained. A late
        // keeper response is not a local cancellation completion fence.
        playerOwner->quiesce();
        hungKeeper.reset();
        // deletes the recording service before the stores it reads
        keeperEngine->finalize();
    }

    chl::ServiceId keeperServiceId(uint16_t provider_id)
    {
        std::string const self = keeperEngine->self();
        auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        return chl::ServiceId(GetParam(), "127.0.0.1", port, provider_id);
    }

    std::unique_ptr<chl::KeeperHotFetchClient> clientOf(uint16_t provider_id)
    {
        return std::unique_ptr<chl::KeeperHotFetchClient>(
                chl::KeeperHotFetchClient::CreateKeeperHotFetchClient(playerOwner,
                                                                      keeperServiceId(provider_id),
                                                                      kDeadline));
    }

    chl::IngestionQueue ingestionQueue;
    chl::StoryChunkExtractionQueue extractionQueue;
    chl::KeeperChunkRetentionStore retentionStore{extractionQueue, 0};
    std::unique_ptr<tl::engine> keeperEngine;
    std::unique_ptr<tl::engine> playerEngine;
    unsigned runtimeCapacity = 2;
    std::shared_ptr<chl::PlaybackRpcOwner> playerOwner;
    std::shared_ptr<chl::rpc::RpcRuntime> runtime;
    std::unique_ptr<HungKeeper> hungKeeper;
    std::unique_ptr<chl::KeeperHotFetchClient> liveClient;
    std::unique_ptr<chl::KeeperHotFetchClient> hungClient;
};
} // namespace

TEST_P(KeeperHotFetch, LiveKeeperReturnsItsRetainedRange)
{
    liveClient = clientOf(kLiveKeeperProvider);
    ASSERT_NE(liveClient, nullptr);

    chl::HotRangeResponse response = liveClient->fetchRange(kStory, 0, 1000, 100);
    EXPECT_TRUE(response.answered);
    // the chunk was never sent to a grapher, so it comes back unconfirmed
    EXPECT_TRUE(response.events.empty());
    ASSERT_EQ(response.unconfirmed_events.size(), 1u);
    EXPECT_EQ(response.unconfirmed_events.front().time(), 150u);
    EXPECT_EQ(response.hot_floor, 150u);
}

// Two replays can race to create the client of one keeper; the player keeps one
// and deletes the other. The RPC they call is registered once per engine, so
// deleting a client must leave every other client able to fetch.
TEST_P(KeeperHotFetch, DeletingOneClientLeavesTheOthersWorking)
{
    liveClient = clientOf(kLiveKeeperProvider);
    ASSERT_NE(liveClient, nullptr);
    {
        auto loser = clientOf(kLiveKeeperProvider);
        ASSERT_NE(loser, nullptr);
    }

    chl::HotRangeResponse response = liveClient->fetchRange(kStory, 0, 1000, 100);
    EXPECT_TRUE(response.answered);
    EXPECT_EQ(response.unconfirmed_events.size(), 1u);
}

TEST_P(KeeperHotFetch, FetchesIssuedTogetherCostOneDeadlineNotTwo)
{
    // the player asks every keeper of a story; issuing the fetches before
    // waiting on any of them keeps one silent keeper from adding its deadline
    // to every other keeper's
    hungClient.reset(chl::KeeperHotFetchClient::CreateKeeperHotFetchClient(playerOwner,
                                                                           keeperServiceId(kHungKeeperProvider),
                                                                           5 * kDeadline));
    ASSERT_NE(hungClient, nullptr);

    auto const started = std::chrono::steady_clock::now();
    auto replay = chl::rpc::RpcDeadline::fromStart(started, 5 * kDeadline);
    auto deadline = chl::KeeperHotFetchClientTestAccess::deadline(replay, started, kDeadline);
    EXPECT_EQ(started + kDeadline, deadline.expiresAt());
    auto one = runtime->reserve(deadline);
    auto two = runtime->reserve(deadline);
    ASSERT_TRUE(one);
    ASSERT_TRUE(two);
    auto issued = std::make_shared<chl::rpc::FanoutIssueBarrier>(2);
    auto first = hungClient->fetchRangeAsync(std::move(one), kStory, 0, 1000, 100, issued);
    auto second = hungClient->fetchRangeAsync(std::move(two), kStory, 0, 1000, 100, issued);
    chl::HotRangeResponse first_response = hungClient->waitForRange(first);
    chl::HotRangeResponse second_response = hungClient->waitForRange(second);
    auto const elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_FALSE(first_response.answered);
    EXPECT_FALSE(second_response.answered);
    // cappedAttempt floors remaining time to positive whole milliseconds.
    // Its earliest native timeout is strictly less than 1 ms before expiry.
    auto const earliest = deadline.expiresAt() - started - std::chrono::milliseconds(1);
    std::fprintf(stderr,
                 "B08 fanout elapsed_ns=%lld derived_floor_ns=%lld upper_ns=%lld active=%d replies=%d\n",
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(earliest).count()),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(2 * kDeadline).count()),
                 hungKeeper->active.load(),
                 hungKeeper->replies.load());
    EXPECT_EQ(2, hungKeeper->active.load());
    EXPECT_EQ(0, hungKeeper->replies.load());
    EXPECT_GT(elapsed, earliest);
    EXPECT_LT(elapsed, 2 * kDeadline);
    EXPECT_FALSE(replay.expired());
}

TEST_P(KeeperHotFetch, HungKeeperCostsTheDeadlineAndDropsOutOfTheMin)
{
    hungClient = clientOf(kHungKeeperProvider);
    ASSERT_NE(hungClient, nullptr);

    auto const started = std::chrono::steady_clock::now();
    chl::HotRangeResponse response = hungClient->fetchRange(kStory, 0, 1000, 100);
    auto const elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, kDeadline + std::chrono::seconds(1));
    EXPECT_EQ(response.hot_floor, UINT64_MAX);
    EXPECT_TRUE(response.events.empty());
    // the player has to tell this apart from a keeper that holds nothing: this
    // keeper's watermark is unknown, and what it has already freed with it
    EXPECT_FALSE(response.answered);
}

// The keeper holds the fetch without answering. The timed forward is
// cancelled and returns, so the fetch gives its slot back while the keeper
// still holds the request, and a late reply changes nothing.
TEST_P(KeeperHotFetch, UnansweredFetchGivesItsSlotBack)
{
    hungClient = clientOf(kHungKeeperProvider);
    ASSERT_NE(hungClient, nullptr);
    auto response = hungClient->fetchRange(kStory, 0, 1000, 100);
    EXPECT_FALSE(response.answered);
    {
        auto deadline = chl::rpc::RpcDeadline::after(std::chrono::seconds(5));
        chl::rpc::RpcReservation first, second;
        while(!(first && second) && !deadline.expired())
        {
            if(!first)
                first = runtime->reserve(deadline);
            if(!second)
                second = runtime->reserve(deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_TRUE(first && second);
        EXPECT_EQ(1, hungKeeper->active.load());
    }
    hungKeeper->released = true;
    auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(hungKeeper->active.load() && std::chrono::steady_clock::now() < limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(0, hungKeeper->active.load());
    auto deadline = chl::rpc::RpcDeadline::after(kDeadline);
    auto first = runtime->reserve(deadline);
    auto second = runtime->reserve(deadline);
    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
}
TEST_P(KeeperHotFetch, QuiescenceRetainsActualEngineProviderAndRejectsNewEntries)
{
    auto provider = std::make_shared<int>(17);
    std::weak_ptr<int> retained = provider;
    playerOwner->provider = provider;
    provider.reset();
    playerOwner->quiesce();
    EXPECT_FALSE(playerOwner->enter());
    EXPECT_EQ(chl::CL_ERR_SHUTDOWN_INCOMPLETE,
              runtime->drain(chl::rpc::RpcDeadline::after(std::chrono::milliseconds(20))));
    EXPECT_FALSE(retained.expired());
    EXPECT_NE(playerOwner->engine, nullptr);
}
class KeeperHotFetchC6: public KeeperHotFetch
{
    void SetUp() override
    {
        runtimeCapacity = 6;
        KeeperHotFetch::SetUp();
    }
};
// An unanswered fetch whose timed forward has returned holds neither a slot
// nor a hot permit, so the player's queries settle while the keeper still
// holds the request.
TEST_P(KeeperHotFetchC6, UnansweredFetchLetsQueriesSettleBeforeTheLatePeerReply)
{
    hungClient = clientOf(kHungKeeperProvider);
    ASSERT_NE(hungClient, nullptr);
    EXPECT_FALSE(hungClient->fetchRange(kStory, 0, 1000, 100).answered);
    ASSERT_EQ(hungKeeper->active.load(), 1); // Actual dispatch, not capacity refusal.
    EXPECT_TRUE(playerOwner->quiesceQueries(chl::rpc::RpcDeadline::after(std::chrono::seconds(5))));
    EXPECT_TRUE(playerOwner->queriesSettled.load());
    EXPECT_EQ(hungKeeper->active.load(), 1);
    hungKeeper->released = true;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(hungKeeper->active.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(hungKeeper->active.load(), 0);
    EXPECT_NE(playerOwner->engine, nullptr);
}
TEST_P(KeeperHotFetchC6, ActivationAfterClosureAndActualCallbackStartNoQueryOrHotFetch)
{
    struct Data
    {
        chl::StoryChunkIngestionQueue ingestion;
        chl::PlayerDataStore store{ingestion};
        chl::ArchiveReadingRequestQueue requests;
    };
    auto data = std::make_shared<Data>();
    playerOwner->dependencies = data;
    data->store.setStoryKeepers(kStory, {keeperServiceId(kHungKeeperProvider)});
    ASSERT_TRUE(playerOwner->quiesceQueries(chl::rpc::RpcDeadline::after(std::chrono::milliseconds(100))));
    auto* service = chl::PlaybackService::CreatePlaybackService(playerOwner, 91, data->store, data->requests);
    ASSERT_NE(service, nullptr);
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto receiver = keeperServiceId(kLiveKeeperProvider);
    // Native test relay invokes the actual production callback with its real
    // request. Its reply acknowledges only this test observation, not a query.
    auto relay = playerOwner->engine->define(
            "test_closed_query_callback",
            [service, data, calls, receiver](tl::request const& request)
            {
                service->story_playback_request(request, receiver, 1, kStory, "chron", "story", 0, 1000);
                ++*calls;
                request.respond(1);
            });
    auto endpoint = playerOwner->engine->lookup(static_cast<std::string>(playerOwner->engine->self()));
    auto deadline = chl::rpc::RpcDeadline::after(std::chrono::seconds(1));
    auto attempt = deadline.cappedAttempt(std::chrono::seconds(1));
    ASSERT_TRUE(attempt);
    int observed = relay.on(endpoint).timed(*attempt);
    EXPECT_EQ(observed, 1);
    EXPECT_EQ(calls->load(), 1u);
    EXPECT_FALSE(playerOwner->enter());
    EXPECT_EQ(hungKeeper->active.load(), 0);
    EXPECT_EQ(playerOwner->queryPermits.used(), 0u);
    EXPECT_EQ(playerOwner->hotPermits.used(), 0u);
    EXPECT_TRUE(chl::rpc::allControlIdle(*runtime, 6, chl::rpc::RpcDeadline::after(std::chrono::milliseconds(100))));
}
INSTANTIATE_TEST_SUITE_P(Transports, KeeperHotFetchC6, ::testing::Values("ofi+sockets", "ofi+tcp"));
INSTANTIATE_TEST_SUITE_P(Transports, KeeperHotFetch, ::testing::Values("ofi+sockets", "ofi+tcp"));
