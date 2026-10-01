#include <gtest/gtest.h>
#include <absl/crc/crc32c.h>

#include <algorithm>
#include <csignal>
#include <future>
#include <poll.h>
#include <sys/wait.h>

#include "adapter/ArchiveService.h"
#include "archive/KeeperArchive.h"
#include "chrono-grapher/server/ArchiveService.h"
#include "membership/ConfigMembership.h"
#include "wal_harness.h"

namespace chronolog::test
{
namespace
{
using namespace std::chrono_literals;
using keeper::KeeperArchive;

struct ArchiveRig
{
    WalRig wal;
    keeper::ConfigMembership membership{{{1, {7, {{"self", "self:1"}}, "127.0.0.1:1", ""}}}};
    KeeperArchive::Time now{};
    keeper::KeeperArchiveConfig config;
    std::unique_ptr<KeeperArchive> archive;
    ArchiveRig()
    {
        config.story_chunk_duration_secs = 1;
        config.archive_visibility_delay_secs = 0;
        reset();
    }
    void reset()
    {
        archive = std::make_unique<KeeperArchive>(*wal.current, membership, "keeper", config, [this] { return now; });
    }
    void
    append(uint64_t sequence, int64_t physical, size_t payload_size = 8, Durability durability = Durability::Durable)
    {
        wal.clock->setPhysical(physical);
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = sequence;
        item.envelope.payload.assign(payload_size, 'x');
        auto result = wal.current->append({1, 7, {item}}, durability);
        ASSERT_TRUE(result.ok());
        ASSERT_TRUE(result->front().status.ok());
    }
    Chunk sealFirst()
    {
        append(1, 100'000'000);
        wal.clock->setPhysical(1'000'000'000);
        EXPECT_TRUE(archive->seal().ok());
        return archive->chunks().front();
    }
    void deliver(const Chunk& chunk, const std::string& instance = "g1", uint64_t receipt_number = 5)
    {
        internal::v1::ChunkReceipt receipt;
        receipt.set_chunk_id(chunk.id);
        receipt.set_bytes(123);
        receipt.set_grapher_instance(instance);
        receipt.set_receipt(receipt_number);
        archive->delivered(chunk.id, receipt, 123);
    }
    void
    report(Hlc watermark, const std::string& instance = "g1", uint64_t highest = 5, std::vector<uint64_t> pending = {})
    {
        archive->applyReport({1, watermark, instance, highest, std::move(pending), false});
    }
    size_t events()
    {
        auto result = wal.current->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
        EXPECT_TRUE(result.ok());
        return result.ok() ? result->size() : 0;
    }
};

TEST(KeeperRetention, FreeOrderShippedThenWatermarkThenTailRelease)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.deliver(chunk);
    rig.report(chunk.end);
    EXPECT_EQ(rig.events(), 1u);
    rig.archive->releaseTail(1);
    EXPECT_EQ(rig.events(), 0u);
}
TEST(KeeperRetention, FreeOrderWatermarkThenShippedThenTailRelease)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.report(chunk.end);
    rig.deliver(chunk);
    EXPECT_EQ(rig.events(), 1u);
    rig.archive->releaseTail(1);
    EXPECT_EQ(rig.events(), 0u);
}
TEST(KeeperRetention, FreeOrderTailReleaseThenWatermarkThenShipped)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.archive->releaseTail(1);
    rig.report(chunk.end);
    EXPECT_EQ(rig.events(), 1u);
    rig.deliver(chunk);
    EXPECT_EQ(rig.events(), 0u);
}
TEST(KeeperRetention, CapacityEvictionKeepsChunkRetainedUntilDurable)
{
    ArchiveRig rig;
    rig.config.retention_cap_mb = 1;
    rig.reset();
    rig.append(1, 100'000'000, 1u << 20);
    rig.wal.clock->setPhysical(1'000'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    rig.append(2, 1'100'000'000, 1u << 20);
    rig.wal.clock->setPhysical(2'000'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    rig.archive->sweep();
    EXPECT_EQ(rig.events(), 2u);
    EXPECT_EQ(rig.archive->chunks().size(), 2u);
    auto first = rig.archive->chunks().front();
    rig.deliver(first);
    rig.report(first.end);
    rig.archive->sweep();
    EXPECT_EQ(rig.events(), 1u);
    EXPECT_EQ(rig.archive->chunks().size(), 1u);
}
TEST(KeeperRetention, MarkSendFailedKeepsChunkReadableAndResendable)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.archive->sendFailed(chunk.id);
    EXPECT_EQ(rig.events(), 1u);
    EXPECT_EQ(rig.archive->chunks().front().id, chunk.id);
    rig.now += 1s;
    rig.deliver(chunk);
    rig.report(chunk.end);
    rig.archive->releaseTail(1);
    EXPECT_EQ(rig.events(), 0u);
}
TEST(KeeperRetention, WatermarkRegressionIsIgnored)
{
    ArchiveRig rig;
    rig.sealFirst();
    rig.report({5'000'000'000, 0});
    rig.report({1'000'000'000, 0});
    EXPECT_EQ(rig.archive->knownWatermark(1), (Hlc{5'000'000'000, 0}));
    EXPECT_EQ(rig.events(), 1u);
}
TEST(KeeperRetention, ChunkAckedUnderACoveringWatermarkWaitsForItsReceipt)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.deliver(chunk);
    rig.archive->releaseTail(1);
    rig.report(chunk.end, "g1", 5, {5});
    EXPECT_EQ(rig.events(), 1u);
    rig.report(chunk.end);
    EXPECT_EQ(rig.events(), 0u);
}
TEST(KeeperRetention, RestartedGrapherSettlesAResentChunkBelowTheKnownWatermark)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.deliver(chunk);
    rig.archive->releaseTail(1);
    rig.report({10'000'000'000, 0}, "g1", 5, {5});
    EXPECT_EQ(rig.events(), 1u);
    rig.deliver(chunk, "g2", 1);
    rig.report({100, 0}, "g2", 1);
    EXPECT_EQ(rig.events(), 0u);
}
TEST(KeeperRetention, DelayedReportFromTheSameGrapherDoesNotUndoANewerOne)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.deliver(chunk);
    rig.report({5'000'000'000, 0});
    rig.report({3'000'000'000, 0}, "g1", 5, {5});
    rig.report(chunk.end, "g1", 4);
    rig.archive->releaseTail(1);
    EXPECT_EQ(rig.events(), 0u);
}
TEST(KeeperRetention, DropReportFreesEveryChunkOfTheStory)
{
    ArchiveRig rig;
    rig.sealFirst();
    rig.append(2, 1'100'000'000);
    rig.wal.clock->setPhysical(2'000'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    rig.archive->applyReport({1, {}, "g1", 0, {}, true});
    EXPECT_TRUE(rig.archive->chunks().empty());
    EXPECT_EQ(rig.events(), 0u);
    EXPECT_EQ(rig.wal.current->evictionFloor(1), (Hlc{2'000'000'000, 0}));
}
TEST(KeeperRetention, DurableChunkIsFreedOnlyOnceTheVisibilityDelayHasPassed)
{
    ArchiveRig rig;
    rig.config.archive_visibility_delay_secs = 10;
    rig.reset();
    auto chunk = rig.sealFirst();
    rig.deliver(chunk);
    rig.report(chunk.end);
    rig.archive->releaseTail(1);
    rig.now += 9s;
    rig.archive->sweep();
    EXPECT_EQ(rig.events(), 1u);
    rig.now += 1s;
    rig.archive->sweep();
    EXPECT_EQ(rig.events(), 0u);
}
TEST(KeeperChunks, ChainStaysContiguousAcrossEmptyStretch)
{
    ArchiveRig rig;
    auto first = rig.sealFirst();
    rig.wal.clock->setPhysical(5'000'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    ASSERT_EQ(rig.archive->chunks().size(), 1u);
    rig.append(2, 8'100'000'000);
    rig.wal.clock->setPhysical(9'000'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    auto chunks = rig.archive->chunks();
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[1].start, first.end);
    EXPECT_EQ(chunks[1].end, (Hlc{9'000'000'000, 0}));
}
TEST(KeeperChunks, SplitAtChunkMaxBytesKeepsTheChainContiguous)
{
    ArchiveRig rig;
    rig.config.chunk_max_bytes = 128;
    rig.reset();
    for(uint64_t sequence = 1; sequence <= 3; ++sequence) rig.append(sequence, 100'000'000, 70);
    rig.wal.clock->setPhysical(1'000'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    auto chunks = rig.archive->chunks();
    ASSERT_EQ(chunks.size(), 3u);
    for(size_t i = 1; i < chunks.size(); ++i) EXPECT_EQ(chunks[i].start, chunks[i - 1].end);
    EXPECT_NE(chunks.front().end.logical, 0u);
}
TEST(KeeperChunks, SettledReplayRecoversEvictionFloorAndPreservesWriterState)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.deliver(chunk);
    rig.report(chunk.end);
    rig.archive->releaseTail(1);
    rig.archive.reset();
    rig.wal.reopen();
    rig.reset();
    EXPECT_EQ(rig.wal.current->evictionFloor(1), chunk.end);
    EXPECT_EQ(rig.events(), 0u);
    EXPECT_TRUE(rig.archive->chunks().empty());
    rig.append(2, 2'100'000'000);
    EXPECT_EQ(rig.events(), 1u);
}
TEST(KeeperChunks, SealReplayAfterKillRequeuesIdenticalIdentity)
{
    auto control = std::make_shared<WalControl>();
    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);
    struct Child
    {
        pid_t pid{-1};
        int read_fd{-1};
        ~Child()
        {
            if(pid > 0)
            {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, nullptr, 0);
            }
            if(read_fd >= 0)
                ::close(read_fd);
        }
    } child;
    child.read_fd = pipefd[0];
    child.pid = ::fork();
    ASSERT_GE(child.pid, 0);
    if(child.pid == 0)
    {
        ::close(pipefd[0]);
        ::alarm(10);
        auto clock = std::make_shared<FakeClock>(100'000'000);
        auto membership = std::make_shared<FakeMembership>();
        WalJournalConfig wal_config;
        wal_config.wal_dir = control->directory;
        WalJournal journal(clock, membership, {}, wal_config);
        (void)journal.registerWriter(1, 2, 3);
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = 1;
        item.envelope.payload = "survives";
        (void)journal.append({1, 7, {item}}, Durability::Durable);
        item.sequence = 2;
        (void)journal.append({1, 7, {item}}, Durability::Accepted);
        keeper::KeeperArchiveConfig config;
        config.story_chunk_duration_secs = 1;
        KeeperArchive archive(journal, *membership, "keeper", config);
        clock->setPhysical(1'000'000'000);
        if(!archive.seal().ok())
            ::_exit(2);
        const auto id = archive.chunks().front().id;
        (void)::write(pipefd[1], id.data(), id.size());
        ::pause();
        ::_exit(3);
    }
    ::close(pipefd[1]);
    pollfd ready{pipefd[0], POLLIN, 0};
    ASSERT_EQ(::poll(&ready, 1, 5000), 1);
    char bytes[256];
    auto size = ::read(pipefd[0], bytes, sizeof(bytes));
    ASSERT_GT(size, 0);
    const std::string identity(bytes, static_cast<size_t>(size));
    ASSERT_EQ(::kill(child.pid, SIGKILL), 0);
    ASSERT_EQ(::waitpid(child.pid, nullptr, 0), child.pid);
    child.pid = -1;
    auto clock = std::make_shared<FakeClock>(100);
    auto membership = std::make_shared<FakeMembership>();
    WalJournalConfig config;
    config.wal_dir = control->directory;
    WalJournal journal(clock, membership, {}, config);
    KeeperArchive archive(journal, *membership, "keeper");
    auto chunks = archive.chunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks.front().id, identity);
    auto events = journal.read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->front().id.sequence, 1u);
}
TEST(KeeperChunks, FetchHotReportsEvictedBelow)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    rig.deliver(chunk);
    rig.report(chunk.end);
    rig.archive->releaseTail(1);
    keeper::WorkerPool pool(1, 8);
    keeper::ArchiveService service(*rig.wal.current, rig.membership, pool);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    auto stub = internal::v1::Archive::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    internal::v1::FetchHotRequest request;
    request.set_story_id(1);
    request.mutable_hlc()->mutable_start();
    request.mutable_hlc()->mutable_end()->set_physical_ns(INT64_MAX);
    auto reader = stub->FetchHot(&context, request);
    internal::v1::FetchHotResponse response;
    ASSERT_TRUE(reader->Read(&response));
    ASSERT_TRUE(response.has_trailer());
    EXPECT_EQ(response.trailer().evicted_below().physical_ns(), chunk.end.physical_ns);
    EXPECT_FALSE(reader->Read(&response));
    EXPECT_TRUE(reader->Finish().ok());
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}
} // namespace
} // namespace chronolog::test

namespace chronolog::test
{
namespace
{
class SubscriptionService final: public internal::v1::Archive::Service
{
public:
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::set<StoryId>> subscriptions;
    std::string keeper;
    bool confirm{};
    std::map<StoryId, Hlc> ends;
    uint64_t receipts{};
    grpc::Status WatchWatermarks(grpc::ServerContext* context,
                                 const internal::v1::WatchWatermarksRequest* request,
                                 grpc::ServerWriter<internal::v1::WatchWatermarksResponse>* writer) override
    {
        {
            std::lock_guard lock(mu);
            keeper = request->keeper_id();
            subscriptions.emplace_back(request->story_ids().begin(), request->story_ids().end());
            cv.notify_all();
        }
        for(int round = 0; round < 500 && !context->IsCancelled(); ++round)
        {
            std::vector<internal::v1::WatchWatermarksResponse> reports;
            {
                std::unique_lock lock(mu);
                cv.wait_for(lock, 10ms);
                for(auto story: request->story_ids())
                {
                    auto end = ends.find(story);
                    if(end == ends.end())
                        continue;
                    internal::v1::WatchWatermarksResponse report;
                    report.set_story_id(story);
                    report.mutable_watermark()->set_physical_ns(end->second.physical_ns);
                    report.mutable_watermark()->set_logical(end->second.logical);
                    report.set_grapher_instance("grapher-instance");
                    report.set_highest_receipt(receipts);
                    if(!confirm)
                        for(uint64_t receipt = 1; receipt <= receipts && receipt <= 100; ++receipt)
                            report.add_pending_receipts(receipt);
                    reports.push_back(std::move(report));
                }
            }
            for(const auto& report: reports)
                if(!writer->Write(report))
                    return grpc::Status::OK;
        }
        return grpc::Status::OK;
    }
};

TEST(KeeperTransfer, InvalidReceiptsKeepChunksReadable)
{
    ArchiveRig rig;
    auto chunk = rig.sealFirst();
    internal::v1::ChunkReceipt receipt;
    receipt.set_chunk_id(chunk.id);
    receipt.set_grapher_instance("grapher-instance");
    for(int mode = 0; mode < 3; ++mode)
    {
        receipt.mutable_status()->set_code(mode == 2 ? static_cast<int>(absl::StatusCode::kUnavailable) : 0);
        receipt.set_bytes(mode == 0 ? 124 : 123);
        receipt.set_receipt(mode == 1 ? 0 : 5);
        rig.archive->delivered(chunk.id, receipt, 123);
        EXPECT_EQ(rig.events(), 1u);
        EXPECT_FALSE(rig.wal.current->sealedChunks().front().settled);
    }
}

TEST(KeeperTransfer, RealGrapherPersistsSplitChunksAndConfirmsTheirReceipts)
{
    auto directory = std::make_shared<WalControl>();
    auto store = FileTierStore::Open(directory->directory, "grapher", {{1, {0, 0}}});
    ASSERT_TRUE(store.ok());
    grapher::ArchiveService service(**store, "grapher-instance");
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    ArchiveRig rig;
    rig.config.chunk_max_events = 2;
    rig.config.frame_bytes = 4u << 20;
    rig.config.seal_interval_ms = 10;
    rig.reset();
    rig.membership.setRoute(1, {7, {{"self", "self:1"}}, "127.0.0.1:" + std::to_string(port), ""});
    for(uint64_t sequence = 1; sequence <= 3; ++sequence) rig.append(sequence, 100'000'000, 600u << 10);
    rig.wal.clock->setPhysical(1'000'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    auto chunks = rig.archive->chunks();
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].end, chunks[1].start);
    ASSERT_TRUE(rig.archive->shipOne());
    ASSERT_TRUE(rig.archive->shipOne());
    auto persisted = (*store)->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(persisted.ok());
    ASSERT_EQ(persisted->size(), 3u);
    auto manifest = (*store)->manifest(1);
    ASSERT_TRUE(manifest.ok());
    ASSERT_EQ(manifest->size(), 2u);
    EXPECT_EQ((*manifest)[0].event_count, 2u);
    EXPECT_EQ((*manifest)[1].event_count, 1u);
    for(size_t i = 0; i < persisted->size(); ++i) EXPECT_EQ((*persisted)[i].id.sequence, i + 1);
    rig.archive->start();
    for(int poll = 0; poll < 200 && !rig.archive->chunks().empty(); ++poll) std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(rig.archive->chunks().empty());
    EXPECT_EQ(rig.events(), 0u);
    EXPECT_EQ(rig.wal.current->evictionFloor(1), chunks.back().end);
    rig.archive->stop();
    service.shutdown();
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}

TEST(KeeperTransfer, RealGrapherNotFoundDropsEveryRetainedChunk)
{
    auto directory = std::make_shared<WalControl>();
    auto store = FileTierStore::Open(directory->directory, "grapher", {{1, {0, 0}}});
    ASSERT_TRUE(store.ok());
    grapher::ArchiveService service(**store, "grapher-instance");
    service.dropStory(1);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    ArchiveRig rig;
    rig.membership.setRoute(1, {7, {{"self", "self:1"}}, "127.0.0.1:" + std::to_string(port), ""});
    rig.sealFirst();
    rig.append(2, 1'100'000'000);
    rig.wal.clock->setPhysical(2'000'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    ASSERT_EQ(rig.archive->chunks().size(), 2u);
    ASSERT_TRUE(rig.archive->shipOne());
    EXPECT_TRUE(rig.archive->chunks().empty());
    EXPECT_EQ(rig.events(), 0u);
    EXPECT_EQ(rig.wal.current->evictionFloor(1), (Hlc{2'000'000'000, 0}));
    for(const auto& seal: rig.wal.current->sealedChunks()) EXPECT_TRUE(seal.settled);
    service.shutdown();
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}

TEST(KeeperTransfer, WatermarkWatcherResubscribesWhenRetainedStorySetGrows)
{
    SubscriptionService service;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    const auto endpoint = "127.0.0.1:" + std::to_string(port);
    auto control = std::make_shared<WalControl>();
    auto clock = std::make_shared<FakeClock>(100'000'000);
    auto membership = std::make_shared<keeper::ConfigMembership>(
            std::vector<keeper::StaticRoute>{{1, {7, {{"self", "self:1"}}, endpoint, ""}},
                                             {2, {7, {{"self", "self:1"}}, endpoint, ""}}});
    WalJournalConfig wal_config;
    wal_config.wal_dir = control->directory;
    WalJournal journal(clock, membership, {}, wal_config);
    ASSERT_TRUE(journal.registerWriter(1, 2, 3).ok());
    ASSERT_TRUE(journal.registerWriter(2, 2, 3).ok());
    keeper::KeeperArchiveConfig config;
    config.story_chunk_duration_secs = 1;
    config.seal_interval_ms = 10;
    config.archive_visibility_delay_secs = 0;
    KeeperArchive archive(journal, *membership, "keeper-process", config);
    AppendItem item;
    item.writer_id = 2;
    item.incarnation = 3;
    item.sequence = 1;
    item.envelope.payload = "event";
    auto first = journal.append({1, 7, {item}}, Durability::Durable);
    ASSERT_TRUE(first.ok());
    clock->setPhysical(1'000'000'000);
    ASSERT_TRUE(archive.seal().ok());
    auto acknowledge = [&](StoryId story)
    {
        auto chunks = archive.chunks();
        for(const auto& chunk: chunks)
        {
            if(chunk.story_id != story)
                continue;
            internal::v1::ChunkReceipt receipt;
            receipt.set_chunk_id(chunk.id);
            receipt.set_grapher_instance("grapher-instance");
            receipt.set_bytes(123);
            receipt.set_receipt(story);
            archive.delivered(chunk.id, receipt, 123);
            std::lock_guard lock(service.mu);
            service.ends[story] = chunk.end;
            service.receipts = std::max(service.receipts, story);
            service.cv.notify_all();
        }
    };
    acknowledge(1);
    archive.start();
    {
        std::unique_lock lock(service.mu);
        ASSERT_TRUE(service.cv.wait_for(lock, 5s, [&] { return !service.subscriptions.empty(); }));
        EXPECT_EQ(service.keeper, "keeper-process");
        EXPECT_EQ(service.subscriptions.front(), (std::set<StoryId>{1}));
    }
    clock->setPhysical(1'100'000'000);
    auto second = journal.append({2, 7, {item}}, Durability::Durable);
    ASSERT_TRUE(second.ok());
    clock->setPhysical(2'000'000'000);
    ASSERT_TRUE(archive.seal().ok());
    acknowledge(2);
    {
        std::unique_lock lock(service.mu);
        ASSERT_TRUE(service.cv.wait_for(lock,
                                        5s,
                                        [&]
                                        {
                                            return std::any_of(service.subscriptions.begin(),
                                                               service.subscriptions.end(),
                                                               [](const auto& stories)
                                                               { return stories == std::set<StoryId>{1, 2}; });
                                        }));
        service.confirm = true;
        service.cv.notify_all();
    }
    for(int poll = 0; poll < 100 && !archive.chunks().empty(); ++poll) std::this_thread::sleep_for(10ms);
    EXPECT_TRUE(archive.chunks().empty());
    archive.stop();
    EXPECT_EQ(journal.evictionFloor(1), (Hlc{1'000'000'000, 0}));
    EXPECT_EQ(journal.evictionFloor(2), (Hlc{2'000'000'000, 0}));
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}
} // namespace
} // namespace chronolog::test

namespace chronolog::test
{
TEST(KeeperRetention, ShutdownWaitEndsWhenTheGrapherConfirmsEveryChunk)
{
    auto directory = std::make_shared<WalControl>();
    auto store = FileTierStore::Open(directory->directory, "grapher", {{1, {0, 0}}});
    ASSERT_TRUE(store.ok());
    grapher::ArchiveService service(**store, "shutdown-grapher");
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    ArchiveRig rig;
    rig.config.shutdown_confirm_timeout_secs = 3;
    rig.config.archive_visibility_delay_secs = 600;
    rig.reset();
    rig.membership.setRoute(1, {7, {{"self", "self:1"}}, "127.0.0.1:" + std::to_string(port), ""});
    rig.append(1, 100'000'000);
    ASSERT_TRUE(rig.archive->seal().ok());
    ASSERT_TRUE(rig.archive->chunks().empty());
    EXPECT_TRUE(rig.archive->shutdown());
    auto events = (*store)->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 1u);
    ASSERT_EQ(rig.wal.current->sealedChunks().size(), 1u);
    EXPECT_TRUE(rig.wal.current->sealedChunks().front().settled);
    EXPECT_EQ(rig.events(), 1u);
    service.shutdown();
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}

TEST(KeeperRetention, ShutdownWaitSendsAgainAChunkTheGrapherNeverWrote)
{
    auto directory = std::make_shared<WalControl>();
    auto store = FileTierStore::Open(directory->directory, "grapher", {{1, {0, 0}}});
    ASSERT_TRUE(store.ok());
    grapher::ArchiveService service(**store, "restarted-grapher");
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    ArchiveRig rig;
    rig.config.shutdown_confirm_timeout_secs = 3;
    rig.reset();
    rig.membership.setRoute(1, {7, {{"self", "self:1"}}, "127.0.0.1:" + std::to_string(port), ""});
    const auto chunk = rig.sealFirst();
    rig.deliver(chunk, "lost-grapher", 5);
    EXPECT_TRUE(rig.archive->shutdown());
    auto events = (*store)->read(1, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events->size(), 1u);
    EXPECT_TRUE(rig.wal.current->sealedChunks().front().settled);
    service.shutdown();
    server->Shutdown(std::chrono::system_clock::now() + 2s);
}

TEST(KeeperRetention, ShutdownWaitGivesUpAtTheTimeout)
{
    ArchiveRig rig;
    rig.config.shutdown_confirm_timeout_secs = 1;
    rig.reset();
    rig.sealFirst();
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(rig.archive->shutdown());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, 1s);
    EXPECT_LT(elapsed, 2s);
    EXPECT_EQ(rig.events(), 1u);
    EXPECT_FALSE(rig.wal.current->sealedChunks().front().settled);
}
} // namespace chronolog::test

namespace chronolog::test
{
TEST(KeeperRetention, ShutdownSealsPendingDurableAppendsAfterFsync)
{
    ArchiveRig rig;
    rig.config.shutdown_confirm_timeout_secs = 0;
    rig.reset();
    rig.wal.control->block();
    auto append = std::async(std::launch::async, [&] { rig.append(1, 100'000'000); });
    (void)rig.wal.control->waitPending();
    auto shutdown = std::async(std::launch::async, [&] { return rig.archive->shutdown(); });
    EXPECT_EQ(shutdown.wait_for(20ms), std::future_status::timeout);
    rig.wal.control->release();
    ASSERT_EQ(append.wait_for(5s), std::future_status::ready);
    append.get();
    ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
    EXPECT_FALSE(shutdown.get());
    const auto seals = rig.wal.current->sealedChunks();
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(rig.events(), 1u);
    EXPECT_FALSE(seals.front().settled);
}
} // namespace chronolog::test
