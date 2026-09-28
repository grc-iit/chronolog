// Transport test for the grapher -> keeper watermark report: the grapher's
// WatermarkReportPublisher and the keeper's DataStoreAdminService, talking
// over real engines on the loopback interface. The publisher must send each
// keeper one coalesced map holding only the stories that keeper contributed
// to, and the keeper must apply it to its retention store, which is what lets
// it free chunks that are durable in the archive.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <abt.h>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <ServiceId.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionQueue.h>

#include <IngestionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperDataStore.h>
// the grapher has a DataStoreAdminService.h of its own; name the keeper's
#include <chrono-keeper/include/DataStoreAdminService.h>

#include <StoryWatermarkRegistry.h>
#include <WatermarkReportPublisher.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kKeeper1Provider = 11;
constexpr uint16_t kKeeper2Provider = 12;

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "watermark_report_transport_test_logger");
        done = true;
    }
}

// Initialized once and never finalized, so margo leaves Argobots alone at
// engine finalize and the keeper objects can still log (tl::thread::self_id())
// from their destructors.
void ensureArgobots()
{
    static bool done = false;
    if(!done)
    {
        ABT_init(0, nullptr);
        done = true;
    }
}

// A keeper's watermark receive path: the data store the admin service
// forwards to, and the retention store behind it.
struct KeeperSide
{
    chl::IngestionQueue ingestionQueue;
    chl::StoryChunkExtractionQueue extractionQueue;
    chl::KeeperChunkRetentionStore retentionStore{extractionQueue, 0};
    chl::KeeperDataStore dataStore{ingestionQueue, extractionQueue, retentionStore};
};

// Reports are one-way, so delivery is only observable through its effect.
bool waitFor(std::function<bool()> const& condition, std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while(!condition())
    {
        if(std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

class WatermarkReportTransport: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        ensureArgobots();
        keeperEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        grapherEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        keeper1 = startKeeper(kKeeper1Provider, keeper1Id);
        keeper2 = startKeeper(kKeeper2Provider, keeper2Id);
    }

    void TearDown() override
    {
        publisher.reset();
        grapherEngine->finalize();
        // deletes the admin service providers before the data stores they use
        keeperEngine->finalize();
        keeper1.reset();
        keeper2.reset();
    }

    std::unique_ptr<KeeperSide> startKeeper(uint16_t provider_id, chl::ServiceId& service_id)
    {
        auto keeper = std::make_unique<KeeperSide>();
        chl::DataStoreAdminService::CreateDataStoreAdminService(*keeperEngine, provider_id, keeper->dataStore);
        // the identity a keeper puts on its chunks, built from the engine's
        // actual address as the keeper does
        std::string const self = keeperEngine->self();
        auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        service_id = chl::ServiceId(kProtocol, "127.0.0.1", port, provider_id);
        return keeper;
    }

    void startPublisher(uint32_t report_interval_secs)
    {
        publisher = std::make_unique<chl::WatermarkReportPublisher>(*grapherEngine, registry, report_interval_secs);
    }

    std::unique_ptr<tl::engine> keeperEngine;
    std::unique_ptr<tl::engine> grapherEngine;
    std::unique_ptr<KeeperSide> keeper1;
    std::unique_ptr<KeeperSide> keeper2;
    chl::ServiceId keeper1Id;
    chl::ServiceId keeper2Id;
    chl::StoryWatermarkRegistry registry;
    std::unique_ptr<chl::WatermarkReportPublisher> publisher;
};
} // namespace

TEST_F(WatermarkReportTransport, ReportFreesCoveredChunkOnTheKeeper)
{
    startPublisher(0);
    constexpr chl::StoryId kStory = 5;
    // the keeper sealed and shipped [100, 200) and holds it until W covers it
    auto* chunk = new chl::StoryChunk("chron", "story", kStory, 100, 200);
    chunk->insertEvent(chl::LogEvent(kStory, 150, 1, 0, "event"));
    keeper1->retentionStore.ingestSealedChunk(kStory, chunk);
    keeper1->retentionStore.markShipped(keeper1->extractionQueue.ejectStoryChunk());
    ASSERT_EQ(keeper1->retentionStore.retainedChunkCount(kStory), 1u);

    registry.registerStory(kStory, 100);
    publisher->recordContributor(kStory, keeper1Id);
    registry.advancePersisted(kStory, 100, 200);
    publisher->publish();

    EXPECT_TRUE(waitFor([&] { return keeper1->retentionStore.retainedChunkCount(kStory) == 0; }));
    EXPECT_EQ(keeper1->retentionStore.knownPersisted(kStory), 200u);
}

TEST_F(WatermarkReportTransport, EachKeeperReceivesOnlyItsOwnStories)
{
    startPublisher(0);
    // Ids ascend so kShared sorts last in every keeper's report. The keeper
    // applies a report in map order, so once kShared has landed, anything
    // misrouted into the same report has landed too.
    constexpr chl::StoryId kNoContributor = 1;
    constexpr chl::StoryId kOnlyKeeper2 = 2;
    constexpr chl::StoryId kOnlyKeeper1 = 3;
    constexpr chl::StoryId kShared = 4;
    for(chl::StoryId story: {kNoContributor, kOnlyKeeper2, kOnlyKeeper1, kShared})
    {
        registry.registerStory(story, 100);
        registry.advancePersisted(story, 100, 100 + 100 * story); // W = 100 + 100 * id
    }
    publisher->recordContributor(kOnlyKeeper1, keeper1Id);
    publisher->recordContributor(kShared, keeper1Id);
    publisher->recordContributor(kOnlyKeeper2, keeper2Id);
    publisher->recordContributor(kShared, keeper2Id);
    publisher->publish();

    auto& store1 = keeper1->retentionStore;
    auto& store2 = keeper2->retentionStore;
    ASSERT_TRUE(
            waitFor([&] { return store1.knownPersisted(kShared) == 500 && store2.knownPersisted(kShared) == 500; }));
    EXPECT_EQ(store1.knownPersisted(kOnlyKeeper1), 400u);
    EXPECT_EQ(store2.knownPersisted(kOnlyKeeper2), 300u);
    EXPECT_EQ(store1.knownPersisted(kOnlyKeeper2), 0u);
    EXPECT_EQ(store2.knownPersisted(kOnlyKeeper1), 0u);
    EXPECT_EQ(store1.knownPersisted(kNoContributor), 0u);
    EXPECT_EQ(store2.knownPersisted(kNoContributor), 0u);
}

TEST_F(WatermarkReportTransport, PublishWithinTheIntervalSendsNothing)
{
    startPublisher(1);
    constexpr chl::StoryId kStory = 5;
    registry.registerStory(kStory, 100);
    publisher->recordContributor(kStory, keeper1Id);
    registry.advancePersisted(kStory, 100, 200);

    // the interval counts from construction, so this round is skipped...
    publisher->publish();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(keeper1->retentionStore.knownPersisted(kStory), 0u);

    // ...and the advance is still pending for the first round after it
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    publisher->publish();
    EXPECT_TRUE(waitFor([&] { return keeper1->retentionStore.knownPersisted(kStory) == 200; }));
}
