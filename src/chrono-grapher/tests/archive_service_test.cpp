#include "chrono-grapher/server/ArchiveService.h"
#include "chrono-grapher/server/GrapherConfig.h"
#include <absl/crc/crc32c.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
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
    Server()
    {
        root = std::filesystem::temp_directory_path() /
               ("chronolog_archive_" + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root);
        auto opened = FileTierStore::Open(root, "test-writer", {{1, {100, 0}}});
        if(!opened.ok())
            throw std::runtime_error(std::string(opened.status().message()));
        store = *std::move(opened);
        service = std::make_unique<ArchiveService>(*store, "test-instance", TransferLimits{4096, 2048, 2});
        grpc::ServerBuilder builder;
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

TEST(ArchiveTransferTest, PartialStreamYieldsNoReceipt)
{
    Server server;
    auto frame = Frame();
    frame.set_data(frame.data().substr(0, frame.data().size() / 2));
    frame.set_final(false);
    auto [status, receipt] = Send(server, {frame});
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(receipt.receipt(), 0u);
    EXPECT_TRUE(server.store->manifest(1)->empty());
}

TEST(ArchiveTransferTest, ChecksumMismatchYieldsNoReceipt)
{
    Server server;
    auto frame = Frame();
    auto checksum = frame.checksum();
    checksum[0] ^= 1;
    frame.set_checksum(checksum);
    auto [status, receipt] = Send(server, {frame});
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(receipt.receipt(), 0u);
    EXPECT_TRUE(server.store->manifest(1)->empty());
}

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
    server.service->dropStory(1);
    ASSERT_TRUE(watch->Read(&report));
    EXPECT_TRUE(report.dropped());
    EXPECT_EQ(report.highest_receipt(), receipt.receipt());
    context.TryCancel();
    EXPECT_FALSE(watch->Finish().ok());
    auto [dropped_status, dropped_receipt] = Send(server, {Frame()});
    EXPECT_EQ(dropped_status.error_code(), grpc::StatusCode::NOT_FOUND);
    EXPECT_EQ(dropped_receipt.receipt(), 0u);
}

TEST(GrapherConfigTest, ValidatesLimitsAndInternalBindGuard)
{
    auto loaded = GrapherConfig::load(std::nullopt);
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded->manifest_writer, loaded->process_id);
    loaded->internal_listen = "0.0.0.0:50063";
    EXPECT_FALSE(loaded->validate().ok());
    loaded->insecure_bind_all = true;
    EXPECT_TRUE(loaded->validate().ok());
    loaded->limits.chunk_bytes = 0;
    EXPECT_FALSE(loaded->validate().ok());
}
} // namespace
} // namespace chronolog::grapher
