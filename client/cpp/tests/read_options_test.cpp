#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <mutex>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using chronolog::Hlc;
using namespace std::chrono_literals;

class PagePeer final
    : public wire::Catalog::Service
    , public wire::Replay::Service
{
public:
    explicit PagePeer(bool tombstoned = false, bool mismatched_epoch = false)
        : tombstoned_(tombstoned)
        , mismatched_epoch_(mismatched_epoch)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Replay::Service*>(this));
        server_ = builder.BuildAndStart();
        endpoint_ = "127.0.0.1:" + std::to_string(port);
    }
    ~PagePeer() override
    {
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        server_->Wait();
    }
    sdk::ClientOptions options() const
    {
        sdk::ClientOptions options;
        options.catalog_endpoint = endpoint_;
        options.rpc_timeout = 2s;
        return options;
    }
    std::vector<wire::ReadRequest> requests()
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }
    grpc::Status
    GetStory(grpc::ServerContext*, const wire::GetStoryRequest* request, wire::GetStoryResponse* p) override
    {
        auto* story = p->mutable_story();
        story->set_story_id(request->story_id());
        story->set_epoch(3);
        story->set_tombstoned(tombstoned_);
        story->mutable_route()->set_epoch(mismatched_epoch_ ? 2 : 3);
        story->mutable_route()->set_player(endpoint_);
        auto* keeper = story->mutable_route()->add_keepers();
        keeper->set_process_id("keeper");
        keeper->set_endpoint(endpoint_);
        return grpc::Status::OK;
    }
    grpc::Status Read(grpc::ServerContext*,
                      const wire::ReadRequest* request,
                      grpc::ServerWriter<wire::ReadResponse>* stream) override
    {
        {
            std::lock_guard lock(mutex_);
            requests_.push_back(*request);
        }
        const bool first_page = request->hlc().start().physical_ns() == 0;
        const bool paged = request->max_events() != 0;
        wire::ReadResponse response;
        for(uint64_t sequence = first_page ? 1 : 3; sequence <= (paged && first_page ? 2u : 3u); ++sequence)
        {
            auto* event = response.mutable_batch()->add_events();
            event->mutable_id()->set_story_id(request->story_id());
            event->mutable_id()->set_writer_id(1);
            event->mutable_id()->set_incarnation(1);
            event->mutable_id()->set_sequence(sequence);
            event->mutable_hlc()->set_physical_ns(sequence < 3 ? 10 : 20);
        }
        stream->Write(response);
        response.Clear();
        auto* completion = response.mutable_completion();
        completion->set_complete(!paged || !first_page);
        completion->set_reason(paged && first_page ? wire::INCOMPLETE_REASON_TRUNCATED
                                                   : wire::INCOMPLETE_REASON_UNSPECIFIED);
        completion->mutable_frontier()->set_physical_ns(paged && first_page ? 20 : 40);
        stream->Write(response);
        return grpc::Status::OK;
    }

private:
    const bool tombstoned_;
    const bool mismatched_epoch_;
    std::string endpoint_;
    std::unique_ptr<grpc::Server> server_;
    std::mutex mutex_;
    std::vector<wire::ReadRequest> requests_;
};

TEST(ClientRead, MaxEventsIsSentAndPagesCertify)
{
    PagePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    sdk::HlcRange range{{0, 0}, {40, 0}};
    std::vector<chronolog::Event> events;
    for(int page = 0; page < 2; ++page)
    {
        auto read = client->read(1, range, sdk::ReadOptions{1});
        ASSERT_TRUE(read.ok()) << read.status();
        auto batch = read->next();
        ASSERT_TRUE(batch.ok()) << batch.status();
        ASSERT_TRUE(*batch);
        EXPECT_EQ((**batch).events.size(), page == 0 ? 2u : 1u);
        for(const auto& event: (**batch).events)
        {
            EXPECT_GE(event.hlc, range.start);
            events.push_back(event);
        }
        auto trailer = read->next();
        ASSERT_TRUE(trailer.ok()) << trailer.status();
        ASSERT_TRUE(*trailer);
        ASSERT_TRUE((**trailer).completion);
        const auto& completion = *(**trailer).completion;
        EXPECT_EQ(completion.complete, page == 1);
        EXPECT_EQ(completion.frontier, page == 0 ? (Hlc{20, 0}) : range.end);
        EXPECT_EQ(completion.reason,
                  page == 0 ? chronolog::IncompleteReason::Truncated : chronolog::IncompleteReason::None);
        for(const auto& event: (**batch).events) EXPECT_LT(event.hlc, completion.frontier);
        if(page == 0)
        {
            ASSERT_TRUE((**trailer).continuation);
            EXPECT_EQ(*(**trailer).continuation, completion.frontier);
            range.start = *(**trailer).continuation;
        }
        else
            EXPECT_FALSE((**trailer).continuation);
        auto eof = read->next();
        ASSERT_TRUE(eof.ok()) << eof.status();
        EXPECT_FALSE(*eof);
    }
    ASSERT_EQ(events.size(), 3u);
    for(size_t i = 0; i < events.size(); ++i)
    {
        EXPECT_EQ(events[i].id.sequence, i + 1);
        if(i)
            EXPECT_TRUE(chronolog::ReplayLess(events[i - 1], events[i]));
    }
    auto requests = peer.requests();
    ASSERT_EQ(requests.size(), 2u);
    for(const auto& request: requests) EXPECT_EQ(request.max_events(), 1u);
    EXPECT_EQ(requests[0].hlc().start().physical_ns(), 0);
    EXPECT_EQ(requests[1].hlc().start().physical_ns(), 20);
}

TEST(ClientRead, ExistingAndZeroOptionsUseThePlayerDefault)
{
    PagePeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    for(int mode = 0; mode < 3; ++mode)
    {
        const sdk::HlcRange range{{0, 0}, {40, 0}};
        auto read = mode == 0 ? client->read(1, range, std::chrono::system_clock::now() + 2s)
                              : client->read(1, range, mode == 1 ? sdk::ReadOptions{} : sdk::ReadOptions{0});
        ASSERT_TRUE(read.ok()) << read.status();
        auto batch = read->next();
        ASSERT_TRUE(batch.ok()) << batch.status();
        ASSERT_TRUE(*batch);
        EXPECT_EQ((**batch).events.size(), 3u);
        auto trailer = read->next();
        ASSERT_TRUE(trailer.ok()) << trailer.status();
        ASSERT_TRUE(*trailer);
        ASSERT_TRUE((**trailer).completion);
        EXPECT_TRUE((**trailer).completion->complete);
        EXPECT_FALSE((**trailer).continuation);
    }
    auto requests = peer.requests();
    ASSERT_EQ(requests.size(), 3u);
    for(const auto& request: requests) EXPECT_EQ(request.max_events(), 0u);
}

TEST(ClientRead, RouteExposesLiveStoryMetadataAndRejectsInvalidSnapshots)
{
    for(int mode = 0; mode < 3; ++mode)
    {
        PagePeer peer(mode == 1, mode == 2);
        auto client = sdk::Client::Connect(peer.options());
        ASSERT_TRUE(client.ok()) << client.status();
        auto route = client->route(1);
        if(mode == 0)
        {
            ASSERT_TRUE(route.ok()) << route.status();
            EXPECT_EQ(route->epoch, 3u);
            EXPECT_EQ(route->player, peer.options().catalog_endpoint);
            ASSERT_EQ(route->keepers.size(), 1u);
            EXPECT_EQ(route->keepers.front().process_id, "keeper");
        }
        else
            EXPECT_EQ(route.status().code(),
                      mode == 1 ? absl::StatusCode::kFailedPrecondition : absl::StatusCode::kDataLoss);
        EXPECT_EQ(client->route(0).status().code(), absl::StatusCode::kInvalidArgument);
        EXPECT_TRUE(peer.requests().empty());
    }
}
} // namespace
