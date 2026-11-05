// Transport test for the grapher -> keeper watermark report: the grapher's
// WatermarkReportPublisher and the keeper's DataStoreAdminService, talking
// over real engines on the loopback interface. The publisher must send each
// keeper one coalesced map holding only the stories that keeper contributed
// to, and the keeper must apply it to its retention store, which is what lets
// it free chunks that are durable in the archive. The receipts that tie a
// chunk to its write travel both ways: in the grapher's answer to a chunk and
// in the report.

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <sstream>

#include <abt.h>
#include <cereal/archives/binary.hpp>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <ChunkReceipt.h>
#include <RDMATransferAgent.h>
#include <ServiceId.h>
#include <StoryChunk.h>
#include <StoryChunkExtractionQueue.h>

#include <IngestionQueue.h>
#include <KeeperChunkRetentionStore.h>
#include <KeeperDataStore.h>
// the grapher has a DataStoreAdminService.h of its own; name the keeper's
#include <chrono-keeper/include/DataStoreAdminService.h>

#include <ChunkIngestionQueue.h>
#include <GrapherRecordingService.h>
#include <StoryChunkIngestionHandle.h>
#include <StoryWatermarkRegistry.h>
#include <WatermarkReportPublisher.h>

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kKeeper1Provider = 11;
constexpr uint16_t kKeeper2Provider = 12;
constexpr uint16_t kGrapherRecordingProvider = 21;

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

// Receipt callbacks retain this heap state through the actual owners. The
// engine fields are the original owning objects moved out of the base fixture,
// never copied wrappers. There are no owner/runtime back-links here.
struct ReceiptIngestion
{
    std::mutex mutex;
    chl::StoryChunkDeque active, passive;
    chl::ChunkIngestionQueue queue;
    std::unique_ptr<chl::StoryChunkIngestionHandle> handle;
    ~ReceiptIngestion()
    {
        for(auto* chunk: active) delete chunk;
        for(auto* chunk: passive) delete chunk;
    }
};
struct ReceiptDependencies
{
    std::shared_ptr<KeeperSide> keeper1, keeper2;
    chl::StoryWatermarkRegistry registry;
    ReceiptIngestion ingestion;
    // Last declared: on a pre-exposure failure engines retire before the stores.
    std::shared_ptr<tl::engine> keeperEngine, grapherEngine;
};
struct ReceiptPause
{
    std::atomic<bool> allocated{false}, release{false};
};
struct ReceiptTransfer
{
    std::shared_ptr<ReceiptDependencies> dependencies;
    std::shared_ptr<chl::rpc::RpcRuntime> sendRuntime, receiveRuntime;
    std::shared_ptr<chl::rpc::RpcBulkOwner> sendOwner, receiveOwner;
    std::unique_ptr<chl::RDMATransferAgent> sender;
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
                std::make_shared<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        grapherEngine =
                std::make_shared<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        keeper1 = startKeeper(kKeeper1Provider, keeper1Id);
        keeper2 = startKeeper(kKeeper2Provider, keeper2Id);
    }

    void TearDown() override
    {
        publisher.reset();
        if(grapherEngine && !reportOwner)
            grapherEngine->finalize();
        // deletes the admin service providers before the data stores they use
        if(keeperEngine && !reportOwner)
            keeperEngine->finalize();
        keeper1.reset();
        keeper2.reset();
    }

    std::shared_ptr<KeeperSide> startKeeper(uint16_t provider_id, chl::ServiceId& service_id)
    {
        auto keeper = std::make_shared<KeeperSide>();
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
        reportRuntime = std::make_shared<chl::rpc::RpcRuntime>();
        reportOwner = chl::rpc::RpcBulkOwner::Create(grapherEngine, reportRuntime, {}, false);
        struct Dependencies
        {
            std::shared_ptr<tl::engine> keeperEngine;
            std::shared_ptr<KeeperSide> keeper1, keeper2;
        };
        reportOwner->dependencies = std::make_shared<Dependencies>(Dependencies{keeperEngine, keeper1, keeper2});
        publisher = std::make_unique<chl::WatermarkReportPublisher>(reportOwner, registryOwner, report_interval_secs);
    }

    std::shared_ptr<tl::engine> keeperEngine;
    std::shared_ptr<tl::engine> grapherEngine;
    std::shared_ptr<KeeperSide> keeper1;
    std::shared_ptr<KeeperSide> keeper2;
    chl::ServiceId keeper1Id;
    chl::ServiceId keeper2Id;
    std::shared_ptr<chl::StoryWatermarkRegistry> registryOwner = std::make_shared<chl::StoryWatermarkRegistry>();
    chl::StoryWatermarkRegistry& registry = *registryOwner;
    std::shared_ptr<chl::rpc::RpcRuntime> reportRuntime;
    std::shared_ptr<chl::rpc::RpcBulkOwner> reportOwner;
    std::unique_ptr<chl::WatermarkReportPublisher> publisher;

    std::shared_ptr<ReceiptTransfer> startReceiptTransfer(chl::RpcTimeoutPolicy policy = {},
                                                          std::shared_ptr<ReceiptPause> pause = {})
    {
        auto transfer = std::make_shared<ReceiptTransfer>();
        auto dependencies = std::make_shared<ReceiptDependencies>();
        transfer->dependencies = dependencies;
        // shared_ptr conversion can allocate. Keep stores in the original
        // fixture until both actual owning engines have moved successfully.
        dependencies->keeperEngine = std::move(keeperEngine);
        dependencies->grapherEngine = std::move(grapherEngine);
        dependencies->keeper1 = std::move(keeper1);
        dependencies->keeper2 = std::move(keeper2);
        transfer->sendRuntime = std::make_shared<chl::rpc::RpcRuntime>();
        transfer->receiveRuntime = std::make_shared<chl::rpc::RpcRuntime>();
        transfer->sendOwner =
                chl::rpc::RpcBulkOwner::Create(dependencies->keeperEngine, transfer->sendRuntime, policy, false);
        transfer->sendOwner->dependencies = dependencies;
        // This MID already exposes keeper admin callbacks. Root their stores
        // before either outbound registration can throw during setup.
        transfer->sendOwner->receiveChunk = dependencies->keeperEngine->define("receive_story_chunk");
        transfer->sendOwner->available = dependencies->keeperEngine->define("receiver_available");
        transfer->receiveOwner =
                chl::rpc::RpcBulkOwner::Create(dependencies->grapherEngine, transfer->receiveRuntime, policy, false);
        transfer->receiveOwner->dependencies = dependencies;
        auto& ingestion = dependencies->ingestion;
        ingestion.handle = std::make_unique<chl::StoryChunkIngestionHandle>(ingestion.mutex,
                                                                            &ingestion.active,
                                                                            &ingestion.passive);
        ingestion.queue.addStoryIngestionHandle(5, ingestion.handle.get());
        if(pause)
        {
            // The barrier retains no engine, owner or callback dependencies;
            // lifetime is provided by the real receive/source contexts.
            auto mid = dependencies->grapherEngine->get_margo_instance();
            transfer->receiveOwner->incoming.observers.allocated = [pause, mid]
            {
                pause->allocated.store(true, std::memory_order_release);
                while(!pause->release.load(std::memory_order_acquire)) margo_thread_sleep(mid, 1);
            };
        }
        chl::GrapherRecordingService::CreateRecordingService(transfer->receiveOwner,
                                                             kGrapherRecordingProvider,
                                                             ingestion.queue,
                                                             nullptr,
                                                             &dependencies->registry);
        std::string const self = dependencies->grapherEngine->self();
        auto port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        transfer->sender.reset(chl::RDMATransferAgent::CreateRDMATransferAgent(
                transfer->sendOwner,
                chl::ServiceId(kProtocol, "127.0.0.1", port, kGrapherRecordingProvider)));
        // These owners intentionally remain process-retained: the fixture has
        // no B12 provider/native retirement proof, even after known receipts.
        // Base TearDown no longer owns or explicitly finalizes their engines.
        return transfer;
    }
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

TEST_F(WatermarkReportTransport, ChunkAckCarriesTheGraphersReceipt)
{
    auto transfer = startReceiptTransfer();
    auto& dependencies = *transfer->dependencies;
    auto& registry = dependencies.registry;
    auto& ingestion = dependencies.ingestion;
    ASSERT_NE(transfer->sender, nullptr);

    // Serialized as the keeper's RDMA extractor does.
    chl::StoryChunk chunk("chron", "story", 5, 100, 200);
    chunk.insertEvent(chl::LogEvent(5, 150, 1, 0, "event"));
    std::ostringstream serialized(std::ios::binary);
    {
        cereal::BinaryOutputArchive archive(serialized);
        archive(chunk);
    }
    auto bytes = serialized.str();
    chl::ChunkReceipt first;
    ASSERT_EQ(transfer->sender->transfer_serialized_story_chunk(bytes, keeper1Id, &first), chl::CL_SUCCESS);
    chl::ChunkReceipt second;
    ASSERT_EQ(transfer->sender->transfer_serialized_story_chunk(bytes, keeper1Id, &second), chl::CL_SUCCESS);

    EXPECT_EQ(first.bytes, bytes.size());
    EXPECT_EQ(second.bytes, bytes.size());
    EXPECT_EQ(first.grapher_instance, registry.instanceId());
    EXPECT_EQ(first.receipt, 1u);
    EXPECT_EQ(second.receipt, 2u);
    transfer->sender.reset();

    // Ingestion precedes the answer and carries both distinct receipt ids.
    ASSERT_TRUE(waitFor(
            [&]
            {
                std::lock_guard<std::mutex> lock(ingestion.mutex);
                return ingestion.active.size() == 2;
            }));
    std::lock_guard<std::mutex> lock(ingestion.mutex);
    std::set<uint64_t> carried;
    for(chl::StoryChunk* received: ingestion.active)
    {
        carried.insert(received->carriedReceipts().begin(), received->carriedReceipts().end());
        delete received;
    }
    ingestion.active.clear();
    EXPECT_EQ(carried, (std::set<uint64_t>{1, 2}));
}

TEST_F(WatermarkReportTransport, UnknownReceiptRetainsActualFixtureThroughTeardownAndLatePull)
{
    chl::RpcTimeoutPolicy policy;
    policy.attempt_ms = 80;
    policy.transfer_ms = 160;
    auto pause = std::make_shared<ReceiptPause>();
    auto transfer = startReceiptTransfer(policy, pause);
    ASSERT_NE(transfer->sender, nullptr);
    std::weak_ptr<ReceiptDependencies> weak = transfer->dependencies;
    std::weak_ptr<chl::rpc::RpcBulkOwner> send = transfer->sendOwner, receive = transfer->receiveOwner;
    chl::StoryChunk chunk("chron", "story", 5, 100, 200);
    chunk.insertEvent(chl::LogEvent(5, 150, 1, 0, "late-owned-event"));
    std::ostringstream stream(std::ios::binary);
    {
        cereal::BinaryOutputArchive archive(stream);
        archive(chunk);
    }
    std::string bytes = stream.str();
    chl::ChunkReceipt receipt;
    ASSERT_EQ(transfer->sender->transfer_serialized_story_chunk(bytes, keeper1Id, &receipt),
              chl::CL_ERR_OUTCOME_UNCONFIRMED);
    ASSERT_TRUE(waitFor([&] { return pause->allocated.load(std::memory_order_acquire); }));
    EXPECT_EQ(receipt.bytes, 0u);
    EXPECT_EQ(transfer->sendOwner->sources.used(), 1u);
    EXPECT_EQ(transfer->receiveOwner->incoming.used(), 1u);
    std::cout << "before fixture teardown: unconfirmed, receipt0, source1, incoming1" << std::endl;
    bytes.assign(bytes.size(), 'X');
    bytes.clear();
    bytes.shrink_to_fit();

    // Reproduce an early ASSERT/fixture exit while the actual pull is paused.
    // Drop every fixture transfer handle first; only charged native owners
    // retain the actual engine objects and all exposed provider dependencies.
    transfer.reset();
    TearDown();
    auto retained = weak.lock();
    ASSERT_NE(retained, nullptr);
    pause->release.store(true, std::memory_order_release);
    ASSERT_TRUE(waitFor(
            [&]
            {
                auto source = send.lock(), target = receive.lock();
                if(!source || !target || source->sources.used() != 0 || target->incoming.used() != 0)
                    return false;
                std::lock_guard<std::mutex> lock(retained->ingestion.mutex);
                return retained->ingestion.active.size() == 1;
            }));
    std::lock_guard<std::mutex> lock(retained->ingestion.mutex);
    auto* received = retained->ingestion.active.front();
    ASSERT_NE(received, nullptr);
    ASSERT_EQ(received->getEventCount(), 1);
    ASSERT_EQ(received->begin()->second.logRecord, "late-owned-event");
    EXPECT_EQ(received->carriedReceipts(), (std::set<uint64_t>{1}));
    delete received;
    retained->ingestion.active.clear();
    std::cout << "after fixture teardown/resume: original event, receipt1, source0, incoming0; native owners "
                 "process-retained"
              << std::endl;
    // This establishes late data/receipt and source/receive settlement only.
    // The native engines/providers remain process-retained, not B12-retired.
}

TEST_F(WatermarkReportTransport, PendingReceiptKeepsTheKeepersChunkUntilItSettles)
{
    startPublisher(0);
    constexpr chl::StoryId kStory = 5;
    registry.registerStory(kStory, 100);
    publisher->recordContributor(kStory, keeper1Id);

    // the keeper shipped [100, 200) and the grapher answered with a receipt
    uint64_t const receipt = registry.assignReceipt(kStory);
    auto* chunk = new chl::StoryChunk("chron", "story", kStory, 100, 200);
    chunk->insertEvent(chl::LogEvent(kStory, 150, 1, 0, "event"));
    keeper1->retentionStore.ingestSealedChunk(kStory, chunk);
    chl::StoryChunk* shipped = keeper1->extractionQueue.ejectStoryChunk();
    shipped->setGrapherReceipt(registry.instanceId(), receipt);
    keeper1->retentionStore.markShipped(shipped);

    // W passes the chunk, but its events went to a window that is not written yet
    registry.holdReceipt(kStory, receipt);
    registry.receiptMerged(kStory, receipt);
    registry.advancePersisted(kStory, 100, 200);
    publisher->publish();
    ASSERT_TRUE(waitFor([&] { return keeper1->retentionStore.knownPersisted(kStory) == 200; }));
    EXPECT_EQ(keeper1->retentionStore.retainedChunkCount(kStory), 1u);

    registry.releaseReceipt(kStory, receipt);
    publisher->publish();
    EXPECT_TRUE(waitFor([&] { return keeper1->retentionStore.retainedChunkCount(kStory) == 0; }));
}

// W can pass a chunk whose receipt is still pending: the chunk arrived while W
// was held below a window already written above it, and its events went into
// that window reopened. Every report has to list the receipt while it is
// pending, including the ones sent before W reached the chunk; a keeper takes a
// receipt a report leaves out for written, and would free the chunk as soon as
// W covers it.
TEST_F(WatermarkReportTransport, ReceiptPendingBeforeWReachesItsChunkStillHoldsTheChunkOnceWDoes)
{
    startPublisher(0);
    constexpr chl::StoryId kStory = 5;
    registry.registerStory(kStory, 100);
    publisher->recordContributor(kStory, keeper1Id);

    // the keeper shipped [200, 300) while W stood at 150
    registry.advancePersisted(kStory, 100, 150);
    uint64_t const receipt = registry.assignReceipt(kStory);
    auto* chunk = new chl::StoryChunk("chron", "story", kStory, 200, 300);
    chunk->insertEvent(chl::LogEvent(kStory, 250, 1, 0, "event"));
    keeper1->retentionStore.ingestSealedChunk(kStory, chunk);
    chl::StoryChunk* shipped = keeper1->extractionQueue.ejectStoryChunk();
    shipped->setGrapherReceipt(registry.instanceId(), receipt);
    keeper1->retentionStore.markShipped(shipped);
    registry.holdReceipt(kStory, receipt);
    registry.receiptMerged(kStory, receipt);
    publisher->publish();
    ASSERT_TRUE(waitFor([&] { return keeper1->retentionStore.knownPersisted(kStory) == 150; }));

    // W passes the chunk; the window holding its events is still unwritten
    registry.advancePersisted(kStory, 150, 400);
    publisher->publish();
    ASSERT_TRUE(waitFor([&] { return keeper1->retentionStore.knownPersisted(kStory) == 400; }));
    EXPECT_EQ(keeper1->retentionStore.retainedChunkCount(kStory), 1u);

    registry.releaseReceipt(kStory, receipt);
    publisher->publish();
    EXPECT_TRUE(waitFor([&] { return keeper1->retentionStore.retainedChunkCount(kStory) == 0; }));
}

// A stale endpoint must not consume canonical progress. Unknown native sends
// retain their slot; a later W remains pending rather than confirming any chunk.
TEST_F(WatermarkReportTransport, FailedReportKeepsLaterCanonicalProgressPending)
{
    startPublisher(0);
    constexpr chl::StoryId kStory = 5;
    registry.registerStory(kStory, 100);
    publisher->recordContributor(kStory, chl::ServiceId(kProtocol, "127.0.0.1", 1, 99));
    registry.advancePersisted(kStory, 100, 200);
    publisher->publish();
    EXPECT_EQ(registry.snapshotDirty().count(kStory), 1u);

    registry.advancePersisted(kStory, 200, 300);
    publisher->publish();
    auto pending = registry.snapshotDirty();
    ASSERT_EQ(pending.count(kStory), 1u);
    EXPECT_EQ(pending.at(kStory).watermark, 300u);
}
