// Unit tests for KeeperHotFetchClient, the player's client of a keeper's
// story_range_fetch. A replay fans out over every keeper of the story and
// splits at the minimum hot floor, so a keeper that never answers must cost
// at most the fetch deadline and then drop out of the minimum; it must not
// hold the replay. Runs over ofi+sockets on 127.0.0.1 against the keeper's
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

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
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
        ABT_init(0, nullptr);
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
        active--;
    }

    std::atomic<bool> released{false};
    std::atomic<int> active{0};
};

class KeeperHotFetch: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        ensureArgobots();
        keeperEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 2);
        playerEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);

        // the live keeper retains one chunk of the story, [100, 200)
        auto* chunk = new chl::StoryChunk("chron", "story", kStory, 100, 200);
        chunk->insertEvent(chl::LogEvent(kStory, 150, 1, 0, "event"));
        retentionStore.ingestSealedChunk(kStory, chunk);
        chl::KeeperRecordingService::CreateKeeperRecordingService(*keeperEngine,
                                                                  kLiveKeeperProvider,
                                                                  ingestionQueue,
                                                                  retentionStore);
        hungKeeper = std::make_unique<HungKeeper>(*keeperEngine, kHungKeeperProvider);
    }

    void TearDown() override
    {
        // let a held request answer while the player engine is still up
        hungKeeper->released = true;
        while(hungKeeper->active > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        liveClient.reset();
        hungClient.reset();
        playerEngine->finalize();
        hungKeeper.reset();
        // deletes the recording service before the stores it reads
        keeperEngine->finalize();
    }

    chl::ServiceId keeperServiceId(uint16_t provider_id)
    {
        std::string const self = keeperEngine->self();
        auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        return chl::ServiceId(kProtocol, "127.0.0.1", port, provider_id);
    }

    std::unique_ptr<chl::KeeperHotFetchClient> clientOf(uint16_t provider_id)
    {
        return std::unique_ptr<chl::KeeperHotFetchClient>(
                chl::KeeperHotFetchClient::CreateKeeperHotFetchClient(*playerEngine,
                                                                      keeperServiceId(provider_id),
                                                                      kDeadline));
    }

    chl::IngestionQueue ingestionQueue;
    chl::StoryChunkExtractionQueue extractionQueue;
    chl::KeeperChunkRetentionStore retentionStore{extractionQueue, 0};
    std::unique_ptr<tl::engine> keeperEngine;
    std::unique_ptr<tl::engine> playerEngine;
    std::unique_ptr<HungKeeper> hungKeeper;
    std::unique_ptr<chl::KeeperHotFetchClient> liveClient;
    std::unique_ptr<chl::KeeperHotFetchClient> hungClient;
};
} // namespace

TEST_F(KeeperHotFetch, LiveKeeperReturnsItsRetainedRange)
{
    liveClient = clientOf(kLiveKeeperProvider);
    ASSERT_NE(liveClient, nullptr);

    chl::HotRangeResponse response = liveClient->fetchRange(kStory, 0, 1000, 100);
    ASSERT_EQ(response.events.size(), 1u);
    EXPECT_EQ(response.events.front().time(), 150u);
    EXPECT_EQ(response.hot_floor, 150u);
}

TEST_F(KeeperHotFetch, HungKeeperCostsTheDeadlineAndDropsOutOfTheMin)
{
    hungClient = clientOf(kHungKeeperProvider);
    ASSERT_NE(hungClient, nullptr);

    auto const started = std::chrono::steady_clock::now();
    chl::HotRangeResponse response = hungClient->fetchRange(kStory, 0, 1000, 100);
    auto const elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, kDeadline + std::chrono::seconds(1));
    EXPECT_EQ(response.hot_floor, UINT64_MAX);
    EXPECT_TRUE(response.events.empty());
}
