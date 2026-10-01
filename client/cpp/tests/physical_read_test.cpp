#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <set>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using namespace std::chrono_literals;
class PhysicalServer final
    : public wire::Catalog::Service
    , public wire::Replay::Service
{
public:
    PhysicalServer()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Replay::Service*>(this));
        server = builder.BuildAndStart();
        endpoint = "127.0.0.1:" + std::to_string(port);
    }
    ~PhysicalServer() { server->Shutdown(std::chrono::system_clock::now() + 2s); }
    grpc::Status GetStory(grpc::ServerContext*, const wire::GetStoryRequest* r, wire::GetStoryResponse* out) override
    {
        ++discovered;
        auto* story = out->mutable_story();
        story->set_story_id(r->story_id());
        story->set_epoch(2);
        story->mutable_route()->set_epoch(2);
        story->mutable_route()->set_player(endpoint);
        return grpc::Status::OK;
    }
    grpc::Status
    Read(grpc::ServerContext*, const wire::ReadRequest* r, grpc::ServerWriter<wire::ReadResponse>* writer) override
    {
        ++reads;
        wire::ReadResponse batch;
        const auto start = r->physical().start_ns(), end = r->physical().end_ns();
        for(int64_t p: {2, 5, 8})
        {
            // The middle event overlaps both sides of a split.
            if(!(p - 1 < end && p + 1 >= start))
                continue;
            if(batch.batch().events_size() == 2)
                break;
            auto* e = batch.mutable_batch()->add_events();
            e->mutable_id()->set_story_id(1);
            e->mutable_id()->set_writer_id(1);
            e->mutable_id()->set_incarnation(1);
            e->mutable_id()->set_sequence(p);
            e->mutable_hlc()->set_physical_ns(p);
        }
        writer->Write(batch);
        wire::ReadResponse final;
        const bool truncated = stuck || end - start > 5;
        final.mutable_completion()->set_complete(!truncated && !incomplete);
        final.mutable_completion()->set_reason(truncated    ? wire::INCOMPLETE_REASON_TRUNCATED
                                               : incomplete ? wire::INCOMPLETE_REASON_LAGGING_WRITERS
                                                            : wire::INCOMPLETE_REASON_UNSPECIFIED);
        writer->Write(final);
        return grpc::Status::OK;
    }
    sdk::ClientOptions options()
    {
        sdk::ClientOptions result;
        result.catalog_endpoint = endpoint;
        result.rpc_timeout = 2s;
        return result;
    }
    std::string endpoint;
    std::unique_ptr<grpc::Server> server;
    std::atomic<int> reads{}, discovered{};
    bool stuck{}, incomplete{};
};
TEST(ClientPhysicalRead, DiscoversPlayerSplitsAndDeduplicatesIntervals)
{
    PhysicalServer server;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto read = client->readPhysical(1, {0, 10});
    ASSERT_TRUE(read.ok());
    std::set<chronolog::EventId> ids;
    size_t events = 0;
    std::optional<chronolog::Completion> completion;
    for(size_t i = 0; i < 20; ++i)
    {
        auto item = read->next();
        ASSERT_TRUE(item.ok());
        if(!*item)
            break;
        for(const auto& e: (**item).events)
        {
            ids.insert(e.id);
            ++events;
        }
        if((**item).completion)
            completion = (**item).completion;
        EXPECT_FALSE((**item).continuation);
    }
    EXPECT_EQ(events, 3);
    EXPECT_EQ(ids.size(), 3);
    ASSERT_TRUE(completion);
    EXPECT_TRUE(completion->complete);
    EXPECT_EQ(server.discovered, 1);
    EXPECT_EQ(server.reads, 3);
}
TEST(ClientPhysicalRead, UnsplittableLeafRemainsTruncated)
{
    PhysicalServer server;
    server.stuck = true;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto read = client->readPhysical(1, {2, 3});
    ASSERT_TRUE(read.ok());
    auto events = read->next();
    ASSERT_TRUE(events.ok());
    auto final = read->next();
    ASSERT_TRUE(final.ok());
    ASSERT_TRUE(*final);
    ASSERT_TRUE((**final).completion);
    EXPECT_FALSE((**final).completion->complete);
    EXPECT_EQ((**final).completion->reason, chronolog::IncompleteReason::Truncated);
    EXPECT_EQ(server.reads, 1);
}
TEST(ClientPhysicalRead, AggregateNeedsEveryLeafComplete)
{
    PhysicalServer server;
    server.incomplete = true;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto read = client->readPhysical(1, {0, 10});
    ASSERT_TRUE(read.ok());
    std::optional<chronolog::Completion> completion;
    for(size_t i = 0; i < 20; ++i)
    {
        auto item = read->next();
        ASSERT_TRUE(item.ok());
        if(!*item)
            break;
        if((**item).completion)
            completion = (**item).completion;
    }
    ASSERT_TRUE(completion);
    EXPECT_FALSE(completion->complete);
    EXPECT_EQ(completion->reason, chronolog::IncompleteReason::LaggingWriters);
}
} // namespace
