#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <absl/strings/cord.h>
#include <atomic>
#include <mutex>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using Reason = sdk::AppendRejection;
using Code = absl::StatusCode;
using namespace std::chrono_literals;

class AppendPeer final
    : public wire::Catalog::Service
    , public wire::Journal::Service
{
public:
    AppendPeer(Code code,
               Reason reason,
               bool redirect = false,
               bool first_succeeds = false,
               bool lose_after_refusal = false)
        : code_(code)
        , reason_(reason)
        , redirect_(redirect)
        , first_succeeds_(first_succeeds)
        , lose_after_refusal_(lose_after_refusal)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Journal::Service*>(this));
        server_ = builder.BuildAndStart();
        endpoint_ = "127.0.0.1:" + std::to_string(port);
    }
    ~AppendPeer() override
    {
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        server_->Wait();
    }
    sdk::ClientOptions options() const
    {
        sdk::ClientOptions options;
        options.catalog_endpoint = endpoint_;
        options.rpc_timeout = 2s;
        options.retry.max_retries = 1;
        options.retry.backoff = 0ms;
        return options;
    }
    void succeed() { failing_ = false; }
    std::vector<wire::AppendItem> seen()
    {
        std::lock_guard lock(mutex_);
        return seen_;
    }
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* request, wire::AcquireResponse* p) override
    {
        p->set_story_id(request->story_id());
        p->set_writer_id(7);
        p->set_incarnation(3);
        p->mutable_route()->set_epoch(1);
        p->mutable_route()->set_player(endpoint_);
        auto* keeper = p->mutable_route()->add_keepers();
        keeper->set_process_id("keeper");
        keeper->set_endpoint(endpoint_);
        *p->mutable_assigned_keeper() = *keeper;
        return grpc::Status::OK;
    }
    template <class Request, class Response>
    void answer(const Request& request, Response& response)
    {
        std::lock_guard lock(mutex_);
        response.set_batch_id(request.batch_id());
        for(const auto& item: request.items())
        {
            seen_.push_back(item);
            auto* result = response.add_results();
            if(failing_ && !(first_succeeds_ && item.sequence() == 1))
            {
                result->mutable_status()->set_code(static_cast<int>(code_));
                result->mutable_status()->set_message("FENCED_SUPERSEDED: diagnostic text is not a reason");
                result->set_rejection(static_cast<wire::AppendRejection>(reason_));
                if(redirect_)
                {
                    auto* route = result->mutable_current_route();
                    route->set_epoch(2);
                    route->set_player(endpoint_);
                    auto* keeper = route->add_keepers();
                    keeper->set_process_id("keeper");
                    keeper->set_endpoint(endpoint_);
                }
                continue;
            }
            result->mutable_id()->set_story_id(request.story_id());
            result->mutable_id()->set_writer_id(item.writer_id());
            result->mutable_id()->set_incarnation(item.incarnation());
            result->mutable_id()->set_sequence(item.sequence());
            result->mutable_assigned_hlc()->set_physical_ns(100 + static_cast<int64_t>(item.sequence()));
            result->set_achieved_durability(request.durability());
        }
    }
    grpc::Status Append(grpc::ServerContext*, const wire::AppendRequest* request, wire::AppendResponse* p) override
    {
        answer(*request, *p);
        if(lose_after_refusal_ && replies_++ > 0)
            return {grpc::StatusCode::UNAVAILABLE, "response lost after route refusal"};
        return grpc::Status::OK;
    }
    grpc::Status
    AppendStream(grpc::ServerContext*,
                 grpc::ServerReaderWriter<wire::AppendStreamResponse, wire::AppendStreamRequest>* stream) override
    {
        wire::AppendStreamRequest request;
        while(stream->Read(&request))
        {
            wire::AppendStreamResponse response;
            answer(request, response);
            if(!stream->Write(response))
                break;
        }
        return grpc::Status::OK;
    }

private:
    const Code code_;
    const Reason reason_;
    const bool redirect_;
    const bool first_succeeds_;
    const bool lose_after_refusal_;
    std::atomic<bool> failing_{true};
    std::atomic<size_t> replies_{};
    std::string endpoint_;
    std::unique_ptr<grpc::Server> server_;
    std::mutex mutex_;
    std::vector<wire::AppendItem> seen_;
};

TEST(ClientAppend, RejectionPayloadOnItemAndWholeCallStatus)
{
    for(uint32_t value = 0; value <= 12; ++value)
    {
        const auto reason = static_cast<Reason>(value);
        const bool definitive = value == 1 || value == 2 || value == 3 || value == 5 || value == 11 || value == 12;
        AppendPeer peer(Code::kFailedPrecondition, reason, true);
        auto client = sdk::Client::Connect(peer.options());
        ASSERT_TRUE(client.ok()) << client.status();
        auto writer = client->acquire(1, "payload");
        ASSERT_TRUE(writer.ok()) << writer.status();
        sdk::AppendSpec spec{{"", "original", "", "", {}}};
        auto result = writer->appendBatch(std::span(&spec, 1));
        const absl::Status* status = nullptr;
        if(definitive)
        {
            ASSERT_TRUE(result.ok()) << result.status();
            ASSERT_EQ(result->size(), 1u);
            ASSERT_FALSE(result->front().ok());
            status = &result->front().status();
            EXPECT_EQ(peer.seen().size(), 1u);
            EXPECT_EQ(writer->acquisition().route.epoch, 1u);
        }
        else
        {
            ASSERT_FALSE(result.ok());
            status = &result.status();
            EXPECT_EQ(peer.seen().size(), 2u);
            const bool route_reason = value == 0 || value == 6 || value == 7 || value == 8 || value == 9;
            EXPECT_EQ(writer->acquisition().route.epoch, route_reason ? 2u : 1u);
        }
        EXPECT_EQ(status->code(), Code::kFailedPrecondition);
        EXPECT_EQ(status->message(), "FENCED_SUPERSEDED: diagnostic text is not a reason");
        EXPECT_TRUE(status->GetPayload("chronolog.dev/append-rejection"));
        EXPECT_EQ(sdk::rejectionOf(*status), reason);
        if(!definitive)
        {
            peer.succeed();
            sdk::AppendSpec different = spec;
            different.envelope.payload = "different";
            EXPECT_EQ(writer->appendBatch(std::span(&different, 1)).status().code(), Code::kFailedPrecondition);
            EXPECT_EQ(peer.seen().size(), 2u);
            auto redriven = writer->appendBatch(std::span(&spec, 1));
            ASSERT_TRUE(redriven.ok()) << redriven.status();
            ASSERT_EQ(redriven->size(), 1u);
            ASSERT_TRUE(redriven->front().ok()) << redriven->front().status();
            EXPECT_EQ(redriven->front()->event_id.sequence, 1u);
        }
        else if(reason == Reason::SequenceGap || reason == Reason::EarlierItemFailed)
        {
            peer.succeed();
            auto next = writer->append({{"", "next", "", "", {}}});
            ASSERT_TRUE(next.ok()) << next.status();
            EXPECT_EQ(next->event_id.sequence, 1u);
        }
    }
    for(Code code: {Code::kInvalidArgument, Code::kOutOfRange})
    {
        AppendPeer peer(code, Reason::Unspecified);
        auto client = sdk::Client::Connect(peer.options());
        ASSERT_TRUE(client.ok()) << client.status();
        auto writer = client->acquire(1, "unspecified payload");
        ASSERT_TRUE(writer.ok()) << writer.status();
        auto result = writer->append({{"", "invalid", "", "", {}}});
        EXPECT_EQ(result.status().code(), code);
        EXPECT_TRUE(result.status().GetPayload("chronolog.dev/append-rejection"));
        EXPECT_EQ(sdk::rejectionOf(result.status()), Reason::Unspecified);
        peer.succeed();
        auto next = writer->append({{"", "next", "", "", {}}});
        ASSERT_TRUE(next.ok()) << next.status();
        EXPECT_EQ(next->event_id.sequence, code == Code::kOutOfRange ? 2u : 1u);
    }
}

TEST(ClientAppend, FsyncUnavailableKeepsThePendingSpec)
{
    for(bool streaming: {false, true})
    {
        AppendPeer peer(Code::kUnavailable, Reason::Unspecified);
        auto client = sdk::Client::Connect(peer.options());
        ASSERT_TRUE(client.ok()) << client.status();
        auto writer = client->acquire(1, "fsync");
        ASSERT_TRUE(writer.ok()) << writer.status();
        sdk::AppendSpec original{{"", "may already be stored", "", "", {}}};
        const auto append = [&](const sdk::AppendSpec& spec) -> absl::StatusOr<sdk::BatchResult>
        {
            if(streaming)
                return writer->appendBatch(std::span(&spec, 1));
            auto receipt = writer->append(spec);
            if(!receipt.ok())
                return receipt.status();
            return sdk::BatchResult{*receipt};
        };
        auto unavailable = append(original);
        EXPECT_EQ(unavailable.status().code(), Code::kUnavailable);
        EXPECT_TRUE(unavailable.status().GetPayload("chronolog.dev/append-rejection"));
        const auto pending = peer.seen();
        ASSERT_EQ(pending.size(), 2u);
        EXPECT_EQ(pending[0].SerializeAsString(), pending[1].SerializeAsString());
        sdk::AppendSpec different = original;
        different.envelope.payload = "must not receive the original receipt";
        peer.succeed();
        auto refused = append(different);
        EXPECT_EQ(refused.status().code(), Code::kFailedPrecondition);
        EXPECT_EQ(peer.seen().size(), pending.size());
        auto redriven = append(original);
        ASSERT_TRUE(redriven.ok()) << redriven.status();
        ASSERT_EQ(redriven->size(), 1u);
        ASSERT_TRUE(redriven->front().ok()) << redriven->front().status();
        EXPECT_EQ(redriven->front()->event_id, (chronolog::EventId{1, 7, 3, 1}));
        EXPECT_EQ(peer.seen().back().SerializeAsString(), pending.front().SerializeAsString());
        auto next = append(different);
        ASSERT_TRUE(next.ok()) << next.status();
        ASSERT_TRUE(next->front().ok());
        EXPECT_EQ(next->front()->event_id.sequence, 2u);
        EXPECT_EQ(peer.seen().back().envelope().payload(), different.envelope.payload);
    }
}

TEST(ClientAppend, LastRouteReasonSurvivesTransportRetryExhaustion)
{
    AppendPeer peer(Code::kFailedPrecondition, Reason::NotRegistered, true, false, true);
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "lost response");
    ASSERT_TRUE(writer.ok()) << writer.status();
    auto result = writer->append({{"", "original", "", "", {}}});
    EXPECT_EQ(result.status().code(), Code::kUnavailable);
    EXPECT_EQ(sdk::rejectionOf(result.status()), Reason::NotRegistered);
    EXPECT_EQ(peer.seen().size(), 2u);
    EXPECT_EQ(writer->append({{"", "different", "", "", {}}}).status().code(), Code::kFailedPrecondition);
    EXPECT_EQ(peer.seen().size(), 2u);
}

TEST(ClientAppend, UnknownReasonIsNotRejected)
{
    const std::vector<std::pair<Code, Reason>> failures{{Code::kFailedPrecondition, static_cast<Reason>(99)},
                                                        {Code::kFailedPrecondition, Reason::DedupeWindow},
                                                        {Code::kFailedPrecondition, Reason::StoryTombstoned},
                                                        {Code::kUnimplemented, Reason::Unspecified},
                                                        {Code::kResourceExhausted, Reason::Unspecified},
                                                        {Code::kInternal, Reason::Unspecified},
                                                        {Code::kDeadlineExceeded, Reason::Unspecified}};
    for(const auto& [code, reason]: failures)
    {
        AppendPeer peer(code, reason, true);
        auto client = sdk::Client::Connect(peer.options());
        ASSERT_TRUE(client.ok()) << client.status();
        auto writer = client->acquire(1, "unknown");
        ASSERT_TRUE(writer.ok()) << writer.status();
        sdk::AppendSpec original{{"", "original", "", "", {}}};
        auto result = writer->append(original);
        EXPECT_EQ(result.status().code(), code);
        EXPECT_EQ(sdk::rejectionOf(result.status()), reason);
        EXPECT_EQ(writer->acquisition().route.epoch, 1u);
        const auto count = peer.seen().size();
        peer.succeed();
        auto refused = writer->append({{"", "different", "", "", {}}});
        EXPECT_EQ(refused.status().code(), Code::kFailedPrecondition);
        EXPECT_EQ(peer.seen().size(), count);
        auto receipt = writer->append(original);
        ASSERT_TRUE(receipt.ok()) << receipt.status();
        EXPECT_EQ(receipt->event_id.sequence, 1u);
    }
}

TEST(ClientAppend, PendingBatchKeepsReceiptsAndRedrivesOnlyUnresolvedItems)
{
    AppendPeer peer(Code::kUnavailable, Reason::Unspecified, false, true);
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "mixed");
    ASSERT_TRUE(writer.ok()) << writer.status();
    std::vector<sdk::AppendSpec> specs{{{"", "successful", "", "", {}}}, {{"", "pending", "", "", {}}}};
    auto result = writer->appendBatch(specs);
    EXPECT_EQ(result.status().code(), Code::kUnavailable);
    const auto before = peer.seen();
    ASSERT_EQ(before.size(), 3u);
    EXPECT_EQ(before[0].sequence(), 1u);
    EXPECT_EQ(before[1].sequence(), 2u);
    EXPECT_EQ(before[2].sequence(), 2u);
    peer.succeed();
    auto changed = specs;
    changed[0].envelope.payload = "different successful item";
    EXPECT_EQ(writer->appendBatch(changed).status().code(), Code::kFailedPrecondition);
    EXPECT_EQ(peer.seen().size(), before.size());
    result = writer->appendBatch(specs);
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->size(), 2u);
    for(size_t i = 0; i < result->size(); ++i)
    {
        ASSERT_TRUE((*result)[i].ok()) << (*result)[i].status();
        EXPECT_EQ((*result)[i]->event_id.sequence, i + 1);
    }
    ASSERT_EQ(peer.seen().size(), 4u);
    EXPECT_EQ(peer.seen().back().sequence(), 2u);
    EXPECT_EQ(peer.seen().back().physical().SerializeAsString(), before[1].physical().SerializeAsString());
    auto next = writer->append({{"", "next", "", "", {}}});
    ASSERT_TRUE(next.ok()) << next.status();
    EXPECT_EQ(next->event_id.sequence, 3u);
}

TEST(ClientAppend, DefinitiveFencesFreezeWritesBeforeRedirect)
{
    for(Reason reason: {Reason::FencedReleased,
                        Reason::FencedSuperseded,
                        Reason::FencedExpired,
                        Reason::FencedOwnerRemoved,
                        Reason::Unspecified})
    {
        AppendPeer peer(Code::kFailedPrecondition, reason, reason != Reason::Unspecified);
        auto client = sdk::Client::Connect(peer.options());
        ASSERT_TRUE(client.ok()) << client.status();
        auto writer = client->acquire(1, "fenced");
        ASSERT_TRUE(writer.ok()) << writer.status();
        sdk::AppendSpec original{{"", "original", "", "", {}}};
        auto result = writer->append(original);
        EXPECT_EQ(result.status().code(), Code::kFailedPrecondition);
        EXPECT_EQ(sdk::rejectionOf(result.status()), reason);
        EXPECT_EQ(writer->acquisition().route.epoch, 1u);
        peer.succeed();
        for(const auto& spec: {original, sdk::AppendSpec{{"", "different", "", "", {}}}})
        {
            result = writer->append(spec);
            EXPECT_EQ(result.status().code(), Code::kFailedPrecondition);
            EXPECT_EQ(sdk::rejectionOf(result.status()), reason);
        }
        EXPECT_EQ(peer.seen().size(), 1u);
    }
}

TEST(ClientAppend, RejectionDecoderIgnoresMessagesAndMalformedPayloads)
{
    auto status = absl::FailedPreconditionError("FENCED_RELEASED");
    EXPECT_EQ(sdk::rejectionOf(status), Reason::Unspecified);
    for(const auto& payload: {std::string{},
                              std::string("1:2"),
                              std::string("\2\0\0\0\1", 5),
                              std::string("\1\0\0\0", 4),
                              std::string("\1\0\0\0\1\0", 6)})
    {
        status.SetPayload("chronolog.dev/append-rejection", absl::Cord(payload));
        EXPECT_EQ(sdk::rejectionOf(status), Reason::Unspecified);
    }
    status.SetPayload("chronolog.dev/append-rejection", absl::Cord(std::string("\1\x80\0\0\x63", 5)));
    EXPECT_EQ(static_cast<uint32_t>(sdk::rejectionOf(status)), 0x80000063u);
}
} // namespace
