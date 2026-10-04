#include <gtest/gtest.h>

#include <atomic>
#include <future>

#include "keeper/adapter/ArchiveService.h"
#include "keeper/adapter/RouteRead.h"
#include "keeper/archive/KeeperArchive.h"
#include "keeper/tests/wal_harness.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "keeper/tests/ram_harness.h"
#include "common/rpc/Channel.h"

namespace chronolog::keeper
{
namespace
{
using namespace std::chrono_literals;

class ReadPeer final
    : public v1::Catalog::Service
    , public internal::v1::Cluster::Service
    , public internal::v1::Archive::Service
{
public:
    std::atomic<unsigned> reads{}, registrations{};
    std::function<void()> reading;
    bool destroyed{};
    std::string endpoint;
    grpc::Status failure;
    std::atomic<unsigned> transfers{};
    grpc::Status TransferChunk(grpc::ServerContext*,
                               grpc::ServerReader<internal::v1::TransferChunkRequest>* stream,
                               internal::v1::TransferChunkResponse* response) override
    {
        internal::v1::TransferChunkRequest frame;
        while(stream->Read(&frame))
        {
            response->set_chunk_id(frame.identity().chunk_id());
            response->set_bytes(frame.total_bytes());
        }
        response->set_grapher_instance("grapher");
        response->set_receipt(++transfers);
        return grpc::Status::OK;
    }
    grpc::Status
    GetStory(grpc::ServerContext*, const v1::GetStoryRequest* request, v1::GetStoryResponse* response) override
    {
        ++reads;
        if(!failure.ok())
            return failure;
        if(reading)
            reading();
        auto* story = response->mutable_story();
        story->set_story_id(request->story_id());
        story->set_tombstoned(destroyed);
        if(!destroyed)
        {
            story->set_epoch(7);
            story->mutable_route()->set_epoch(7);
            story->mutable_route()->set_grapher(endpoint);
            story->mutable_route()->add_keepers()->set_process_id("self");
        }
        return grpc::Status::OK;
    }
    grpc::Status
    Register(grpc::ServerContext*, const internal::v1::RegisterRequest*, internal::v1::RegisterResponse*) override
    {
        ++registrations;
        return grpc::Status::OK;
    }
};

class RouteReadTest: public testing::Test
{
protected:
    void SetUp() override
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<v1::Catalog::Service*>(&peer));
        builder.RegisterService(static_cast<internal::v1::Cluster::Service*>(&peer));
        builder.RegisterService(static_cast<internal::v1::Archive::Service*>(&peer));
        server = builder.BuildAndStart();
        ASSERT_TRUE(server);
        peer.endpoint = "127.0.0.1:" + std::to_string(port);
        catalog = v1::Catalog::NewStub(rpc::peerChannel("127.0.0.1:" + std::to_string(port)));
        membership = std::make_shared<ConfigMembership>(std::vector<StaticRoute>{},
                                                        [this](StoryId id) { return readStoryRoute(*catalog, id); });
        RamJournalConfig config;
        config.process_id = "self";
        journal = std::make_unique<RamJournal>(std::make_shared<test::AssignmentClock>(100), membership, config);
        journal->setRouteResolver(
                [this](StoryId id)
                { return membership->resolve(id, [this, id] { (void)journal->dropStory(id, true); }); });
        ASSERT_TRUE(journal->registerWriter(1, 2, 3).ok());
    }
    void TearDown() override { server->Shutdown(); }
    AppendBatch batch(uint64_t sequence = 1)
    {
        AppendBatch batch;
        batch.story_id = 1;
        batch.epoch = 7;
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = sequence;
        item.envelope.payload = "event";
        item.physical = {100, 1, ClockStatus::Synced};
        batch.items.push_back(item);
        return batch;
    }
    grpc::Status fetch(internal::v1::FetchHotTrailer* trailer = nullptr)
    {
        WorkerPool pool(1, 8);
        ArchiveService service(*journal, *membership, pool);
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        auto serving = builder.BuildAndStart();
        auto stub = internal::v1::Archive::NewStub(rpc::peerChannel("127.0.0.1:" + std::to_string(port)));
        grpc::ClientContext context;
        rpc::withTimeout(context, 5s);
        internal::v1::FetchHotRequest request;
        request.set_story_id(1);
        request.set_expect_epoch(7);
        request.mutable_hlc()->mutable_end()->set_physical_ns(INT64_MAX);
        auto stream = stub->FetchHot(&context, request);
        internal::v1::FetchHotResponse message;
        while(stream->Read(&message))
            if(trailer && message.has_trailer())
                *trailer = message.trailer();
        auto status = stream->Finish();
        serving->Shutdown();
        return status;
    }
    ReadPeer peer;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<v1::Catalog::Stub> catalog;
    std::shared_ptr<ConfigMembership> membership;
    std::unique_ptr<RamJournal> journal;
};

TEST_F(RouteReadTest, FetchHotResolvesMissingRouteAtCatalogEpoch)
{
    EXPECT_EQ(membership->route(1).status().code(), absl::StatusCode::kNotFound);
    internal::v1::FetchHotTrailer trailer;
    EXPECT_TRUE(fetch(&trailer).ok());
    EXPECT_EQ(trailer.epoch(), 7u);
    EXPECT_EQ(peer.reads, 1u);
    EXPECT_EQ(peer.registrations, 0u);
}

TEST_F(RouteReadTest, FetchHotTombstoneDropsStory)
{
    peer.destroyed = true;
    EXPECT_EQ(fetch().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_TRUE(journal->dropped(1));
    EXPECT_EQ(fetch().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(peer.reads, 1u);
}

TEST_F(RouteReadTest, FetchHotFailedLookupIsRetryable)
{
    peer.failure = grpc::Status(grpc::StatusCode::NOT_FOUND, "lookup failed");
    EXPECT_EQ(fetch().error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_FALSE(journal->dropped(1));
}

TEST_F(RouteReadTest, FetchHotSupersededLookupIsRetryable)
{
    peer.reading = [&] { membership->acknowledgeRoutes(30); };
    EXPECT_EQ(fetch().error_code(), grpc::StatusCode::UNAVAILABLE);
    EXPECT_EQ(membership->route(1).status().code(), absl::StatusCode::kNotFound);
    EXPECT_FALSE(journal->dropped(1));
}

TEST_F(RouteReadTest, JournalReadResolvesBeforeTakingStoryLocks)
{
    auto events = journal->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok()) << events.status();
    EXPECT_TRUE(events->empty());
    EXPECT_EQ(peer.reads, 1u);
}

TEST_F(RouteReadTest, RecoveredWalSealsAndTransfersBeforeRouteSnapshot)
{
    test::WalRig wal;
    auto appended = wal.journal->append(batch(), Durability::Durable);
    ASSERT_TRUE(appended.ok());
    ASSERT_TRUE(appended->front().status.ok());
    ASSERT_TRUE(wal.journal->flush().ok());
    wal.journal.reset();
    auto clock = std::make_shared<test::AssignmentClock>(2'000'000'000);
    WalJournal recovered(clock, membership, wal.ram_config, wal.config);
    recovered.setRouteResolver([&](StoryId id)
                               { return membership->resolve(id, [&] { (void)recovered.dropStory(id, true); }); });
    ASSERT_EQ(membership->route(1).status().code(), absl::StatusCode::kNotFound);
    KeeperArchiveConfig config;
    config.story_chunk_duration_secs = 1;
    {
        KeeperArchive archive(recovered, *membership, "self", config);
        ASSERT_TRUE(archive.seal(true).ok());
        ASSERT_EQ(archive.chunks().size(), 1u);
    }
    // Recover the recorded seal with a fresh cache, exercising transfer without seal().
    auto fresh = std::make_shared<ConfigMembership>(std::vector<StaticRoute>{},
                                                    [this](StoryId id) { return readStoryRoute(*catalog, id); });
    recovered.setRouteResolver(nullptr);
    KeeperArchive archive(recovered, *fresh, "self", config);
    recovered.setRouteResolver([&](StoryId id)
                               { return fresh->resolve(id, [&] { (void)recovered.dropStory(id, true); }); });
    ASSERT_EQ(fresh->route(1).status().code(), absl::StatusCode::kNotFound);
    ASSERT_TRUE(archive.shipOne());
    EXPECT_EQ(peer.transfers, 1u);
    EXPECT_EQ(peer.reads, 2u);
    EXPECT_EQ(peer.registrations, 0u);
}

TEST_F(RouteReadTest, CacheMissReadsWithoutRegister)
{
    auto result = journal->append(batch(), Durability::Accepted);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok()) << result->front().status;
    EXPECT_EQ(peer.reads, 1u);
    EXPECT_EQ(peer.registrations, 0u);
    ASSERT_TRUE(journal->append(batch(2), Durability::Accepted).ok());
    EXPECT_EQ(peer.reads, 1u);
}

TEST_F(RouteReadTest, BlockedReadDoesNotHoldStoryGateAndCannotReplaceWatch)
{
    std::promise<void> entered, release;
    auto unblock = release.get_future().share();
    peer.reading = [&]
    {
        entered.set_value();
        unblock.wait();
    };
    auto reading = std::async(std::launch::async, [&] { return journal->append(batch(2), Durability::Accepted); });
    if(entered.get_future().wait_for(5s) != std::future_status::ready)
    {
        release.set_value();
        FAIL() << "read did not enter";
    }
    auto install =
            std::async(std::launch::async,
                       [&]
                       {
                           RouteState state;
                           state.route = Route{7, {{"self", "new"}}, "g", "p"};
                           journal->applyRoute(1, state, false, 20, [&] { membership->setRouteState(1, state, 20); });
                           return journal->append(batch(), Durability::Accepted);
                       });
    auto completed = install.wait_for(5s);
    release.set_value();
    ASSERT_EQ(completed, std::future_status::ready) << "route install/append blocked behind the read";
    auto result = install.get();
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok()) << result->front().status;
    auto first = reading.get();
    ASSERT_TRUE(first.ok());
    EXPECT_TRUE(first->front().status.ok()) << first->front().status;
    ASSERT_TRUE(membership->route(1).ok());
    EXPECT_EQ(membership->route(1)->keepers.front().endpoint, "new");
    EXPECT_EQ(peer.registrations, 0u);
}

TEST_F(RouteReadTest, ReadOlderThanWatchRevisionIsIgnored)
{
    std::promise<void> entered, release;
    auto unblock = release.get_future().share();
    peer.reading = [&]
    {
        entered.set_value();
        unblock.wait();
    };
    auto reading = std::async(std::launch::async, [&] { return membership->resolve(1); });
    auto reached = entered.get_future().wait_for(5s);
    if(reached != std::future_status::ready)
    {
        release.set_value();
        FAIL() << "read did not enter";
    }
    membership->acknowledgeRoutes(30);
    release.set_value();
    EXPECT_EQ(reading.get().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(membership->route(1).status().code(), absl::StatusCode::kNotFound);
}

TEST_F(RouteReadTest, DynamicReadWaitsForMatchingWatchMetadata)
{
    journal->enableDynamic("instance");
    journal->extendCeiling({1000, 0}, 1000);
    RouteState old;
    old.route = Route{6, {{"self", "old"}}, "g", "p"};
    // Model an older watch state whose cache installation has not reached the missing entry.
    journal->applyRoute(1, old, false, 5, [] {});
    auto refused = journal->append(batch(), Durability::Accepted);
    ASSERT_TRUE(refused.ok());
    EXPECT_EQ(refused->front().status.code(), absl::StatusCode::kUnavailable);
    RouteState current;
    current.route = Route{7, {{"self", "new"}}, "g", "p"};
    journal->applyRoute(1, current, false, 6, [&] { membership->setRouteState(1, current, 6); });
    auto accepted = journal->append(batch(), Durability::Accepted);
    ASSERT_TRUE(accepted.ok());
    EXPECT_TRUE(accepted->front().status.ok()) << accepted->front().status;
    EXPECT_EQ(peer.reads, 1u);
    EXPECT_EQ(peer.registrations, 0u);
}

TEST_F(RouteReadTest, GetStoryTombstoneDropsOnceAndIgnoresLaterRoutes)
{
    peer.destroyed = true;
    std::atomic<unsigned> drops{};
    journal->onDrop([&](StoryId) { ++drops; });
    for(unsigned i = 0; i < 2; ++i)
    {
        auto result = journal->append(batch(), Durability::Accepted);
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result->front().rejection, AppendRejection::StoryTombstoned);
    }
    RouteState state;
    state.route.epoch = 100;
    membership->setRouteState(1, state, 100);
    EXPECT_EQ(membership->route(1).status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_TRUE(journal->dropped(1));
    EXPECT_EQ(drops, 1u);
    EXPECT_EQ(peer.reads, 1u);
    EXPECT_EQ(peer.registrations, 0u);
}
} // namespace
} // namespace chronolog::keeper
