#include "rpc/Channel.h"
#include "chrono-grapher/server/ArchiveService.h"
#include "chrono-grapher/server/GrapherConfig.h"
#include "chrono-grapher/server/WorkerPool.h"
#include <absl/crc/crc32c.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace chronolog::grapher
{
namespace
{
namespace wire = internal::v1;

struct Server
{
    std::filesystem::path root;
    std::unique_ptr<FileTierStore> store;
    std::unique_ptr<ArchiveService> service;
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<wire::Archive::Stub> stub;
    explicit Server(std::shared_ptr<const ChunkCodec> codec = std::make_shared<HDF5ChunkCodec>(),
                    FileTierStore::Unlink unlink = {})
    {
        root = std::filesystem::temp_directory_path() /
               ("chronolog_archive_" + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root);
        auto opened = FileTierStore::Open(root, "test-writer", {{1, {100, 0}}}, std::move(codec), std::move(unlink));
        if(!opened.ok())
            throw std::runtime_error(std::string(opened.status().message()));
        store = *std::move(opened);
        service = std::make_unique<ArchiveService>(*store, "test-instance", TransferLimits{4096, 2048, 2});
        grpc::ServerBuilder builder;
        chronolog::rpc::applyServerPolicy(builder);
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(service.get());
        server = builder.BuildAndStart();
        if(!server || !port)
            throw std::runtime_error("cannot start archive test server");
        stub = wire::Archive::NewStub(
                grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    }
    ~Server()
    {
        service->shutdown();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
        server->Wait();
        store.reset();
        std::filesystem::remove_all(root);
    }
};

class FailingCodec final: public ChunkCodec
{
public:
    std::string extension() const override { return inner_.extension(); }
    absl::Status writeChunk(const std::filesystem::path& file, const Chunk& chunk) const override
    {
        if(fail_.exchange(false))
            return absl::UnavailableError("injected chunk write failure");
        {
            std::unique_lock lock(mu_);
            if(gate_)
            {
                entered_ = true;
                cv_.notify_all();
                cv_.wait(lock, [this] { return !gate_; });
            }
        }
        return inner_.writeChunk(file, chunk);
    }
    absl::Status write(const std::filesystem::path& file, std::span<const Event> events) const override
    {
        return inner_.write(file, events);
    }
    absl::StatusOr<std::vector<Event>> read(const std::filesystem::path& file) const override
    {
        return inner_.read(file);
    }
    void failNext() { fail_ = true; }
    void closeGate()
    {
        std::lock_guard lock(mu_);
        gate_ = true;
    }
    bool waitEntered()
    {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, std::chrono::seconds(5), [this] { return entered_; });
    }
    void release()
    {
        {
            std::lock_guard lock(mu_);
            gate_ = false;
        }
        cv_.notify_all();
    }

private:
    ProtoChunkCodec inner_;
    mutable std::atomic<bool> fail_{false};
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    mutable bool gate_{}, entered_{};
};

std::string Crc(const std::string& bytes)
{
    const auto crc = static_cast<uint32_t>(absl::ComputeCrc32c(bytes));
    std::string result;
    for(int shift: {24, 16, 8, 0}) result += static_cast<char>((crc >> shift) & 255);
    return result;
}

wire::TransferChunkRequest Frame(StoryId story = 1)
{
    wire::ChunkPayload payload;
    auto* event = payload.add_events();
    event->mutable_id()->set_story_id(story);
    event->mutable_id()->set_writer_id(2);
    event->mutable_id()->set_incarnation(3);
    event->mutable_id()->set_sequence(1);
    event->mutable_hlc()->set_physical_ns(100);
    event->mutable_hlc()->set_logical(1);
    event->mutable_envelope()->set_payload("payload");
    event->mutable_envelope()->set_content_type("text/plain");
    event->mutable_physical()->set_physical_ns(99);
    event->mutable_physical()->set_uncertainty_ns(5);
    event->mutable_physical()->set_status(v1::CLOCK_STATUS_SYNCED);
    event->set_durability(v1::DURABILITY_DURABLE);
    wire::TransferChunkRequest frame;
    frame.mutable_identity()->set_story_id(story);
    frame.mutable_identity()->set_chunk_id("chunk-1");
    frame.mutable_identity()->mutable_start()->set_physical_ns(100);
    frame.mutable_identity()->mutable_end()->set_physical_ns(200);
    frame.set_data(payload.SerializeAsString());
    frame.set_total_bytes(frame.data().size());
    frame.set_checksum(Crc(frame.data()));
    frame.set_final(true);
    return frame;
}

std::pair<grpc::Status, wire::TransferChunkResponse> Send(Server& server,
                                                          const std::vector<wire::TransferChunkRequest>& frames)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    wire::TransferChunkResponse response;
    auto stream = server.stub->TransferChunk(&context, &response);
    for(const auto& frame: frames)
        if(!stream->Write(frame))
            break;
    stream->WritesDone();
    return {stream->Finish(), response};
}

TEST(ArchiveTransferTest, PublishesBeforeReturningAnInstanceScopedReceipt)
{
    Server server;
    auto first = Frame();
    auto last = first;
    const auto split = first.data().size() / 2;
    first.set_data(first.data().substr(0, split));
    first.set_final(false);
    last.set_data(last.data().substr(split));
    last.set_offset(split);
    auto [status, receipt] = Send(server, {first, last});
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(receipt.status().code(), 0);
    EXPECT_EQ(receipt.chunk_id(), "chunk-1");
    EXPECT_EQ(receipt.bytes(), first.total_bytes());
    EXPECT_EQ(receipt.grapher_instance(), "test-instance");
    EXPECT_EQ(receipt.receipt(), 1u);
    auto events = server.store->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}});
    ASSERT_TRUE(events.ok());
    ASSERT_EQ(events->size(), 1u);
    EXPECT_EQ(events->at(0).envelope.payload, "payload");
    EXPECT_EQ(events->at(0).physical.uncertainty_ns, 5u);
    EXPECT_EQ(events->at(0).durability, Durability::Durable);
    auto other = Frame(2);
    other.set_checksum_algorithm(wire::CHECKSUM_ALGORITHM_CRC32C);
    auto [other_status, other_receipt] = Send(server, {other});
    ASSERT_TRUE(other_status.ok());
    EXPECT_EQ(other_receipt.receipt(), 2u);
    EXPECT_EQ(other_receipt.grapher_instance(), receipt.grapher_instance());
}

#include "../../../tests/contract/archive/archive_transfer_test.cpp"

TEST(ArchiveTransferTest, MalformedFramesNeverPublish)
{
    Server server;
    const auto valid = Frame();
    std::vector<std::vector<wire::TransferChunkRequest>> cases;
    auto frame = valid;
    frame.set_offset(1);
    cases.push_back({frame});
    frame = valid;
    frame.set_total_bytes(5000);
    cases.push_back({frame});
    frame = valid;
    frame.set_data(std::string(2049, 'x'));
    frame.set_total_bytes(2049);
    frame.set_checksum(Crc(frame.data()));
    cases.push_back({frame});
    cases.push_back({valid, valid});
    frame = valid;
    frame.set_final(false);
    cases.push_back({frame});
    frame = valid;
    frame.set_data(frame.data().substr(0, 2));
    cases.push_back({frame});
    auto first = valid;
    first.set_data(first.data().substr(0, 2));
    first.set_final(false);
    frame = valid;
    frame.set_data(frame.data().substr(2));
    frame.set_offset(2);
    frame.mutable_identity()->set_chunk_id("changed");
    cases.push_back({first, frame});
    for(const auto& frames: cases)
    {
        auto [status, receipt] = Send(server, frames);
        EXPECT_FALSE(status.ok());
        EXPECT_EQ(receipt.receipt(), 0u);
    }
    EXPECT_TRUE(server.store->manifest(1)->empty());
    auto [status, receipt] = Send(server, {valid});
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(receipt.receipt(), 1u);
}

TEST(ArchiveWatermarkTest, WatchReflectsPublishAndDroppedStory)
{
    Server server;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    wire::WatchWatermarksRequest subscription;
    subscription.set_keeper_id("keeper-1");
    subscription.add_story_ids(1);
    auto watch = server.stub->WatchWatermarks(&context, subscription);
    wire::WatchWatermarksResponse report;
    ASSERT_TRUE(watch->Read(&report));
    EXPECT_EQ(report.watermark().physical_ns(), 100);
    EXPECT_EQ(report.highest_receipt(), 0u);
    auto [status, receipt] = Send(server, {Frame()});
    ASSERT_TRUE(status.ok());
    bool settled = false;
    for(int i = 0; i < 3 && watch->Read(&report); ++i)
    {
        if(report.watermark().physical_ns() == 200 && report.pending_receipts().empty())
        {
            settled = true;
            break;
        }
    }
    ASSERT_TRUE(settled);
    EXPECT_EQ(report.highest_receipt(), receipt.receipt());
    EXPECT_EQ(report.grapher_instance(), receipt.grapher_instance());
    EXPECT_FALSE(report.dropped());
    server.service->tombstone(1);
    ASSERT_TRUE(watch->Read(&report));
    EXPECT_TRUE(report.dropped());
    EXPECT_EQ(report.highest_receipt(), receipt.receipt());
    context.TryCancel();
    EXPECT_FALSE(watch->Finish().ok());
    auto [dropped_status, dropped_receipt] = Send(server, {Frame()});
    EXPECT_EQ(dropped_status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(dropped_receipt.receipt(), 0u);
}

TEST(ArchiveWatermarkTest, ReceiptStaysPendingUntilTheChunkIsWritten)
{
    auto codec = std::make_shared<FailingCodec>();
    Server server(codec);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    wire::WatchWatermarksRequest subscription;
    subscription.set_keeper_id("keeper-1");
    subscription.add_story_ids(1);
    auto watch = server.stub->WatchWatermarks(&context, subscription);
    wire::WatchWatermarksResponse report;
    ASSERT_TRUE(watch->Read(&report));
    codec->closeGate();
    std::pair<grpc::Status, wire::TransferChunkResponse> sent;
    std::thread sender([&] { sent = Send(server, {Frame()}); });
    ASSERT_TRUE(codec->waitEntered());
    bool pending = false;
    for(int i = 0; i < 5 && !pending && watch->Read(&report); ++i)
        pending = report.pending_receipts_size() == 1 && report.pending_receipts(0) == 1 &&
                  report.highest_receipt() == 1 && report.watermark().physical_ns() == 100;
    codec->release();
    sender.join();
    EXPECT_TRUE(pending);
    ASSERT_TRUE(sent.first.ok());
    EXPECT_EQ(sent.second.receipt(), 1u);
    bool settled = false;
    for(int i = 0; i < 5 && !settled && watch->Read(&report); ++i)
        settled = report.pending_receipts().empty() && report.highest_receipt() == 1 &&
                  report.watermark().physical_ns() == 200;
    EXPECT_TRUE(settled);
    context.TryCancel();
}

TEST(ArchiveTransferTest, AFailedPublishReturnsNoReceiptHoldsTheWatermarkAndNeverReusesTheNumber)
{
    auto codec = std::make_shared<FailingCodec>();
    Server server(codec);
    codec->failNext();
    auto [status, receipt] = Send(server, {Frame()});
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(receipt.receipt(), 0u);
    EXPECT_EQ(server.store->contiguousWatermark(1).value(), (Hlc{100, 0}));
    EXPECT_TRUE(server.store->read(1, {Range::Axis::Hlc, {0, 0}, {1000, 0}})->empty());
    auto [retry_status, retry] = Send(server, {Frame()});
    ASSERT_TRUE(retry_status.ok());
    EXPECT_EQ(retry.receipt(), 2u);
    EXPECT_EQ(server.store->contiguousWatermark(1).value(), (Hlc{200, 0}));
}

TEST(ArchiveTransferTest, AnEmptyWindowGetsAReceiptAndAdvancesTheWatermark)
{
    Server server;
    auto frame = Frame();
    frame.set_data(wire::ChunkPayload().SerializeAsString());
    frame.set_total_bytes(0);
    frame.set_checksum(Crc(""));
    auto [status, receipt] = Send(server, {frame});
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(receipt.receipt(), 1u);
    EXPECT_EQ(server.store->contiguousWatermark(1).value(), (Hlc{200, 0}));
    EXPECT_TRUE(server.store->read(1, {Range::Axis::Hlc, {0, 0}, {1000, 0}})->empty());
}

// A drained Keeper ships an Empty chunk for the eventless part of its own cut (I4.14); its file is freed with the story.
TEST(ArchiveTransferTest, DestroyErasesTheFileOfAnEmptyWindow)
{
    Server server;
    ASSERT_TRUE(Send(server, {Frame()}).first.ok());
    auto empty = Frame();
    empty.mutable_identity()->set_chunk_id("chunk-2");
    empty.mutable_identity()->mutable_start()->set_physical_ns(200);
    empty.mutable_identity()->mutable_end()->set_physical_ns(300);
    empty.set_data(wire::ChunkPayload().SerializeAsString());
    empty.set_total_bytes(0);
    empty.set_checksum(Crc(""));
    ASSERT_TRUE(Send(server, {empty}).first.ok());
    const auto records = server.store->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_EQ(records->size(), 2u);
    EXPECT_EQ(records->back().state, ManifestState::Empty);
    server.service->tombstone(1);
    ASSERT_TRUE(server.service->waitDestroyed(1, std::chrono::seconds(5)));
    for(const auto& record: *records) EXPECT_FALSE(std::filesystem::exists(server.root / record.file)) << record.file;
}

TEST(ArchiveService, ADeletedFileWhoseUnlinkFailedIsRetried)
{
    std::mutex mutex;
    std::condition_variable changed;
    int attempts = 0;
    bool release = false, on_worker = false, retry_timed_out = false;
    Server server(std::make_shared<ProtoChunkCodec>(),
                  [&](const std::filesystem::path& path)
                  {
                      std::unique_lock lock(mutex);
                      ++attempts;
                      if(attempts == 1)
                      {
                          errno = EACCES;
                          return -1;
                      }
                      on_worker = WorkerPool::onWorkerThread();
                      changed.notify_all();
                      if(!changed.wait_for(lock, std::chrono::seconds(5), [&] { return release; }))
                      {
                          retry_timed_out = true;
                          errno = EACCES;
                          return -1;
                      }
                      return ::unlink(path.c_str());
                  });
    ASSERT_TRUE(Send(server, {Frame()}).first.ok());
    ASSERT_TRUE(Send(server, {Frame(2)}).first.ok());
    const auto deleted = server.store->manifest(1)->front();
    const auto published = server.store->manifest(2)->front();
    ASSERT_FALSE(server.store->eraseFile(deleted.file).ok());
    ASSERT_EQ(server.store->manifest(1)->front().state, ManifestState::Deleted);
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return attempts >= 2; }));
        EXPECT_TRUE(on_worker);
    }
    EXPECT_TRUE(std::filesystem::exists(server.root / deleted.file));
    EXPECT_TRUE(std::filesystem::exists(server.root / published.file));
    EXPECT_EQ(server.store->contiguousWatermark(1).value(), (Hlc{200, 0}));
    EXPECT_TRUE(Send(server, {Frame(3)}).first.ok());
    {
        std::lock_guard lock(mutex);
        EXPECT_FALSE(retry_timed_out);
        release = true;
    }
    changed.notify_all();
    server.service->tombstone(1);
    ASSERT_TRUE(server.service->waitDestroyed(1, std::chrono::seconds(5)));
    EXPECT_FALSE(std::filesystem::exists(server.root / deleted.file));
    EXPECT_TRUE(std::filesystem::exists(server.root / published.file));
    EXPECT_EQ(server.store->manifest(2)->front().state, ManifestState::Published);
    EXPECT_EQ(server.store->contiguousWatermark(1).value(), (Hlc{200, 0}));
    auto log = ManifestLog::OpenReadOnly(server.root)->load();
    ASSERT_TRUE(log.ok());
    EXPECT_EQ(std::count_if(log->records.begin(),
                            log->records.end(),
                            [&](const auto& record)
                            { return record.file == deleted.file && record.state == ManifestState::Deleted; }),
              1);
}

TEST(ArchiveWatermarkTest, DroppedStoryRefusesEveryChunkAndIsReportedEvenIfNeverRecorded)
{
    Server server;
    server.service->tombstone(7);
    ASSERT_TRUE(server.service->waitDestroyed(7, std::chrono::seconds(5)));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    wire::WatchWatermarksRequest subscription;
    subscription.set_keeper_id("keeper-1");
    subscription.add_story_ids(7);
    auto watch = server.stub->WatchWatermarks(&context, subscription);
    wire::WatchWatermarksResponse report;
    ASSERT_TRUE(watch->Read(&report));
    EXPECT_TRUE(report.dropped());
    EXPECT_EQ(report.watermark().physical_ns(), 0);
    for(int attempt = 0; attempt < 2; ++attempt)
    {
        auto [status, receipt] = Send(server, {Frame(7)});
        EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
        EXPECT_EQ(receipt.receipt(), 0u);
    }
    auto [other_status, other] = Send(server, {Frame(8)});
    ASSERT_TRUE(other_status.ok());
    EXPECT_EQ(other.receipt(), 1u);
    context.TryCancel();
}

TEST(ArchiveWatermarkTest, TombstoneProducesDroppedReport)
{
    Server server;
    auto [status, receipt] = Send(server, {Frame()});
    ASSERT_TRUE(status.ok());
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    wire::WatchWatermarksRequest subscription;
    subscription.set_keeper_id("keeper-1");
    subscription.add_story_ids(1);
    subscription.add_story_ids(7);
    auto watch = server.stub->WatchWatermarks(&context, subscription);
    wire::WatchWatermarksResponse report;
    std::set<StoryId> dropped;
    for(int i = 0; i < 2 && watch->Read(&report); ++i) EXPECT_FALSE(report.dropped());
    server.service->tombstone(1);
    server.service->tombstone(7);
    while(dropped.size() < 2 && watch->Read(&report))
    {
        if(!report.dropped())
            continue;
        // A dropped report is only ever sent after the Tombstoned record is durable (W10.5, I13.11).
        EXPECT_TRUE(server.store->tombstoned(report.story_id()).value());
        dropped.insert(report.story_id());
        if(report.story_id() == 1)
        {
            EXPECT_EQ(report.watermark().physical_ns(), 200);
            EXPECT_EQ(report.highest_receipt(), receipt.receipt());
        }
        else
            EXPECT_EQ(report.watermark().physical_ns(), 0);
    }
    EXPECT_EQ(dropped, (std::set<StoryId>{1, 7}));
    context.TryCancel();
    grpc::ClientContext late_context;
    late_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    auto late = server.stub->WatchWatermarks(&late_context, subscription);
    for(int i = 0; i < 2; ++i)
    {
        ASSERT_TRUE(late->Read(&report));
        EXPECT_TRUE(report.dropped()) << "a Keeper that subscribes after the tombstone is told at once";
    }
    late_context.TryCancel();
}

TEST(ArchiveWatermarkTest, ARestartedGrapherReportsTheRecoveredWatermarkUnderANewInstance)
{
    Server server;
    auto [first_status, first] = Send(server, {Frame(1)});
    ASSERT_TRUE(first_status.ok());
    auto [second_status, second] = Send(server, {Frame(2)});
    ASSERT_TRUE(second_status.ok());
    EXPECT_EQ(server.store->contiguousWatermark(2).value(), (Hlc{200, 0}));
    ArchiveService restarted(*server.store, "restarted-instance");
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&restarted);
    auto endpoint = builder.BuildAndStart();
    ASSERT_NE(endpoint, nullptr);
    auto stub = wire::Archive::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    wire::WatchWatermarksRequest subscription;
    subscription.set_keeper_id("keeper-1");
    subscription.add_story_ids(1);
    subscription.add_story_ids(2);
    auto watch = stub->WatchWatermarks(&context, subscription);
    wire::WatchWatermarksResponse report;
    std::map<StoryId, int64_t> watermarks;
    for(int i = 0; i < 2 && watch->Read(&report); ++i)
    {
        EXPECT_EQ(report.grapher_instance(), "restarted-instance");
        EXPECT_EQ(report.highest_receipt(), 0u);
        EXPECT_TRUE(report.pending_receipts().empty());
        watermarks[report.story_id()] = report.watermark().physical_ns();
    }
    EXPECT_EQ(watermarks, (std::map<StoryId, int64_t>{{1, 200}, {2, 200}}));
    context.TryCancel();
    restarted.shutdown();
    endpoint->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
}

TEST(ArchiveWatermarkTest, AReportIsSentOnlyWhenItChanges)
{
    Server server;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(800));
    wire::WatchWatermarksRequest subscription;
    subscription.set_keeper_id("keeper-1");
    subscription.add_story_ids(1);
    auto watch = server.stub->WatchWatermarks(&context, subscription);
    wire::WatchWatermarksResponse report;
    ASSERT_TRUE(watch->Read(&report));
    EXPECT_FALSE(watch->Read(&report));
}

TEST(GrapherConfigTest, LogLevelDefaultsToInfoAndRejectsUnknownLevels)
{
    auto loaded = GrapherConfig::load(std::nullopt);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded->log_level, "info");
    loaded->log_level = "error";
    EXPECT_TRUE(loaded->validate().ok());
    loaded->log_level = "debug";
    EXPECT_FALSE(loaded->validate().ok());
}

TEST(GrapherConfigTest, ValidatesLimitsAndInternalBindGuard)
{
    auto loaded = GrapherConfig::load(std::nullopt);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded->archive_codec, "hdf5");
    loaded->archive_codec = "proto";
    EXPECT_TRUE(loaded->validate().ok());
    loaded->archive_codec = "invalid";
    EXPECT_FALSE(loaded->validate().ok());
    loaded->archive_codec = "hdf5";
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded->manifest_writer, loaded->process_id);
    loaded->internal_listen = "0.0.0.0:50063";
    EXPECT_FALSE(loaded->validate().ok());
    loaded->insecure_bind_all = true;
    EXPECT_TRUE(loaded->validate().ok());
    loaded->limits.chunk_bytes = 0;
    EXPECT_FALSE(loaded->validate().ok());
}
Chunk SmallWindow(int index)
{
    const int64_t start = 100 + 50 * index;
    Event event;
    event.id = {1, 2, 3, static_cast<uint64_t>(index + 1)};
    event.hlc = {start, 0};
    event.envelope.payload = "event";
    return {"w" + std::to_string(index), 1, {start, 0}, {start + 50, 0}, {event}, false, false};
}

CompactionSettings EagerCompaction()
{
    CompactionSettings settings;
    settings.enabled = true;
    settings.scan_interval = std::chrono::seconds(1);
    settings.policy.min_files = 2;
    settings.policy.min_age = std::chrono::seconds(0);
    return settings;
}

std::filesystem::path FreshRoot()
{
    auto root =
            std::filesystem::temp_directory_path() / ("chronolog_archive_" + std::to_string(::getpid()) + "_" +
                                                      ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(root);
    return root;
}

TEST(ArchiveTransferTest, CompactionRunsOnItsOwnWorkerOnlyWhenEnabled)
{
    const auto root = FreshRoot();
    auto store = FileTierStore::Open(root, "test-writer", {{1, {100, 0}}});
    ASSERT_TRUE(store.ok());
    for(int i = 0; i < 3; ++i) ASSERT_TRUE((*store)->publish(SmallWindow(i)).ok());
    {
        ArchiveService disabled(**store, "test-instance");
        disabled.shutdown();
    }
    EXPECT_EQ((*store)->manifest(1).value().size(), 3u);
    {
        ArchiveService service(**store, "test-instance", {}, EagerCompaction());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while((*store)->manifest(1).value().size() != 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        service.shutdown();
    }
    const auto records = (*store)->manifest(1).value();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].event_count, 3u);
    EXPECT_EQ((*store)->contiguousWatermark(1).value(), (Hlc{250, 0}));
    store->reset();
    std::filesystem::remove_all(root);
}

TEST(ArchiveTransferTest, DestroyWaitsForCompactionCleanup)
{
    const auto root = FreshRoot();
    FileTierStore::Hooks crash;
    crash.compaction_step = [](std::string_view step)
    { return step == "cleanup" ? absl::AbortedError("injected crash") : absl::OkStatus(); };
    {
        auto writer = FileTierStore::Open(root,
                                          "compactor",
                                          {{1, {100, 0}}},
                                          std::make_shared<HDF5ChunkCodec>(),
                                          {},
                                          {},
                                          0,
                                          {},
                                          crash);
        ASSERT_TRUE(writer.ok());
        for(int i = 0; i < 3; ++i) ASSERT_TRUE((*writer)->publish(SmallWindow(i)).ok());
        auto policy = EagerCompaction().policy;
        EXPECT_FALSE((*writer)->compactOnce(policy).ok());
    }
    // The compacting writer never returns; a peer destroys the story and frees its superseded inputs (I13.11).
    auto store = FileTierStore::Open(root, "test-writer", {{1, {100, 0}}});
    ASSERT_TRUE(store.ok());
    {
        ArchiveService service(**store, "test-instance");
        service.tombstone(1);
        EXPECT_TRUE(service.waitDestroyed(1, std::chrono::seconds(10)));
        service.shutdown();
    }
    EXPECT_TRUE(std::filesystem::is_empty(root / "1"));
    store->reset();
    std::filesystem::remove_all(root);
}

// Bytes of every file under the archive root, which stands in for a file system of a fixed size.
uint64_t BytesUnder(const std::filesystem::path& root)
{
    uint64_t bytes = 0;
    std::error_code error;
    for(std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    {
        std::error_code ignored;
        if(it->is_regular_file(ignored))
            if(const auto size = it->file_size(ignored); !ignored)
                bytes += size;
    }
    return bytes;
}

// I13.16: below hard_stop_reserve_bytes a new window is refused before any file is written, the Keeper sees a failed
// send and keeps its chunk, and the same transfer settles once the reserve is restored. Compaction output is refused
// the same way.
TEST(ArchiveTransferTest, HardStopRefusesNewWindowsAsASendFailure)
{
    Server server;
    server.store->setHardStopReserve(4096, [] { return uint64_t{4095}; });
    auto [status, receipt] = Send(server, {Frame()});
    EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED) << status.error_message();
    EXPECT_EQ(receipt.receipt(), 0u);
    EXPECT_TRUE(server.store->manifest(1)->empty());
    EXPECT_EQ(server.store->contiguousWatermark(1).value(), (Hlc{100, 0}));
    EXPECT_TRUE(!std::filesystem::exists(server.root / "1") || std::filesystem::is_empty(server.root / "1"));
    server.store->setHardStopReserve(4096, [] { return uint64_t{4096}; });
    auto [retry_status, retry] = Send(server, {Frame()});
    ASSERT_TRUE(retry_status.ok()) << retry_status.error_message();
    EXPECT_EQ(retry.receipt(), 2u);
    EXPECT_EQ(server.store->contiguousWatermark(1).value(), (Hlc{200, 0}));
    for(int i = 2; i < 5; ++i) ASSERT_TRUE(server.store->publish(SmallWindow(i)).ok());
    server.store->setHardStopReserve(4096, [] { return uint64_t{0}; });
    EXPECT_EQ(server.store->compactOnce(EagerCompaction().policy).status().code(),
              absl::StatusCode::kResourceExhausted);
    EXPECT_EQ(server.store->manifest(1)->size(), 4u) << "a refused compaction leaves every input effective";
    server.store->setHardStopReserve(4096, [] { return uint64_t{4096}; });
    auto compacted = server.store->compactOnce(EagerCompaction().policy);
    ASSERT_TRUE(compacted.ok()) << compacted.status();
    EXPECT_GE(compacted->inputs, 2u);
}

// The holder path of publish writes no file, so a Keeper resending a settled window still gets its receipt.
TEST(ArchiveTransferTest, HardStopStillSettlesADuplicateTransfer)
{
    Server server;
    auto [status, receipt] = Send(server, {Frame()});
    ASSERT_TRUE(status.ok()) << status.error_message();
    server.store->setHardStopReserve(4096, [] { return uint64_t{0}; });
    auto [again_status, again] = Send(server, {Frame()});
    ASSERT_TRUE(again_status.ok()) << again_status.error_message();
    EXPECT_EQ(again.status().code(), 0);
    EXPECT_EQ(again.chunk_id(), "chunk-1");
    EXPECT_GT(again.receipt(), receipt.receipt());
    auto next = Frame();
    next.mutable_identity()->set_chunk_id("chunk-2");
    next.mutable_identity()->mutable_start()->set_physical_ns(200);
    next.mutable_identity()->mutable_end()->set_physical_ns(300);
    // An Empty window writes a file too, so it is a new window like any other.
    next.set_data(wire::ChunkPayload().SerializeAsString());
    next.set_total_bytes(0);
    next.set_checksum(Crc(""));
    auto [next_status, refused] = Send(server, {next});
    EXPECT_EQ(next_status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED) << next_status.error_message();
    EXPECT_EQ(refused.receipt(), 0u);
    EXPECT_EQ(server.store->manifest(1)->size(), 1u);
    EXPECT_EQ(server.store->contiguousWatermark(1).value(), (Hlc{200, 0}));
}

// Tombstones, Deleted records and unlinks continue inside the reserve, so destroying a story is how a full `local`
// admits new windows again without a restart.
TEST(ArchiveTransferTest, DestroyFreesSpaceAtTheHardStop)
{
    Server server;
    ASSERT_TRUE(Send(server, {Frame()}).first.ok());
    const uint64_t reserve = 1024 * 1024;
    const auto used = BytesUnder(server.root);
    // A file system that holds what the archive holds now and has one byte less than the reserve free.
    const uint64_t capacity = used + reserve - 1;
    server.store->setHardStopReserve(reserve,
                                     [capacity, root = server.root]
                                     {
                                         const auto held = BytesUnder(root);
                                         return capacity > held ? capacity - held : uint64_t{0};
                                     });
    auto [status, receipt] = Send(server, {Frame(2)});
    EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED) << status.error_message();
    EXPECT_EQ(receipt.receipt(), 0u);
    server.service->tombstone(1);
    ASSERT_TRUE(server.service->waitDestroyed(1, std::chrono::seconds(5)));
    EXPECT_LT(BytesUnder(server.root), used) << "the erased chunk outweighs the tombstone and Deleted lines";
    auto [freed_status, freed] = Send(server, {Frame(2)});
    ASSERT_TRUE(freed_status.ok()) << freed_status.error_message();
    EXPECT_NE(freed.receipt(), 0u);
    EXPECT_EQ(server.store->manifest(2)->front().state, ManifestState::Published);
}

TEST(GrapherConfigTest, HardStopReserveDefaultsToTheWalReserveAndZeroDisables)
{
    auto loaded = GrapherConfig::load(std::nullopt);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded->hard_stop_reserve_bytes, 268435456u);
    const auto root = FreshRoot();
    auto store = FileTierStore::Open(root, "test-writer", {{1, {100, 0}}});
    ASSERT_TRUE(store.ok());
    (*store)->setHardStopReserve(0, [] { return uint64_t{0}; });
    EXPECT_TRUE((*store)->publish(SmallWindow(0)).ok());
    // The bound grows with the own manifest and the own effective files (RFC-I 3.7).
    const auto one = (*store)->hardStopReserveBound();
    ASSERT_TRUE(one.ok());
    EXPECT_GT(*one, 0u);
    ASSERT_TRUE((*store)->publish(SmallWindow(1)).ok());
    EXPECT_GT((*store)->hardStopReserveBound().value(), *one);
    store->reset();
    std::filesystem::remove_all(root);
}

TEST(GrapherConfigTest, CompactionIsEnabledByDefaultAndItsKnobsAreValidated)
{
    auto loaded = GrapherConfig::load(std::nullopt);
    ASSERT_TRUE(loaded.ok());
    EXPECT_TRUE(loaded->compaction.enabled);
    EXPECT_EQ(loaded->compaction.policy.min_files, 32u);
    EXPECT_EQ(loaded->compaction.policy.max_files, 128u);
    EXPECT_EQ(loaded->compaction.policy.min_age, std::chrono::seconds(300));
    EXPECT_LE(loaded->compaction.policy.max_events, 262144u / 2);
    loaded->compaction.policy.max_events = 65537;
    EXPECT_FALSE(loaded->validate().ok());
    const auto path =
            std::filesystem::temp_directory_path() / ("chronolog_grapher_config_" + std::to_string(::getpid()));
    std::ofstream(path) << R"({"compact_enabled": false, "compact_min_age_secs": 0, "compact_max_files": 64})";
    loaded = GrapherConfig::load(path.string());
    ASSERT_TRUE(loaded.ok()) << loaded.status();
    EXPECT_FALSE(loaded->compaction.enabled);
    EXPECT_EQ(loaded->compaction.policy.min_age, std::chrono::seconds(0));
    EXPECT_EQ(loaded->compaction.policy.max_files, 64u);
    std::ofstream(path, std::ios::trunc) << R"({"compact_min_files": 0})";
    EXPECT_FALSE(GrapherConfig::load(path.string()).ok());
    std::ofstream(path, std::ios::trunc) << R"({"compact_max_files": 8})";
    EXPECT_FALSE(GrapherConfig::load(path.string()).ok());
    std::filesystem::remove(path);
}
} // namespace
} // namespace chronolog::grapher
