#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <map>
#include <mutex>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using namespace std::chrono_literals;

constexpr int64_t kSlice = 1'000'000;

// One server plays the Catalog and every Keeper. Keeper a starts its hlc counter far ahead of the others, so only
// the causal floor can keep a sequential writer's hlcs increasing when it moves from lane 0 to lane 1.
class LaneServer final
    : public wire::Catalog::Service
    , public wire::Journal::Service
{
public:
    explicit LaneServer(size_t keeper_count)
        : keepers(keeper_count)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Journal::Service*>(this));
        server = builder.BuildAndStart();
        endpoint = "127.0.0.1:" + std::to_string(port);
    }
    ~LaneServer() { server->Shutdown(std::chrono::system_clock::now() + 2s); }
    sdk::ClientOptions options()
    {
        sdk::ClientOptions o;
        o.catalog_endpoint = endpoint;
        o.player_endpoint = endpoint;
        o.rpc_timeout = 2s;
        o.batch_size = 1;
        o.retry.backoff = 1ms;
        o.time_source = [this]
        { return chronolog::TimeReading{clock_ns.load(), 1000, chronolog::ClockStatus::Synced}; };
        return o;
    }
    void fill(wire::Route* route)
    {
        route->set_epoch(2);
        route->set_player(endpoint);
        for(size_t i = 0; i < keepers; ++i)
        {
            auto* k = route->add_keepers();
            k->set_process_id("keeper-" + std::to_string(i));
            k->set_endpoint(endpoint);
        }
    }
    grpc::Status GetStory(grpc::ServerContext*, const wire::GetStoryRequest* r, wire::GetStoryResponse* p) override
    {
        p->mutable_story()->set_story_id(r->story_id());
        p->mutable_story()->set_epoch(2);
        fill(p->mutable_story()->mutable_route());
        return grpc::Status::OK;
    }
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* r, wire::AcquireResponse* p) override
    {
        std::lock_guard lock(mutex);
        acquires.emplace_back(r->writer_identity(), r->preferred_keeper_process_id());
        if(r->writer_identity() == refuse_identity)
            return {grpc::StatusCode::PERMISSION_DENIED, "refused"};
        p->set_story_id(r->story_id());
        p->set_writer_id(acquires.size());
        p->set_incarnation(1);
        fill(p->mutable_route());
        for(const auto& k: p->route().keepers())
            if(k.process_id() == r->preferred_keeper_process_id())
                *p->mutable_assigned_keeper() = k;
        return grpc::Status::OK;
    }
    grpc::Status Release(grpc::ServerContext*, const wire::ReleaseRequest* r, wire::ReleaseResponse* p) override
    {
        std::lock_guard lock(mutex);
        released.push_back(r->writer_id());
        p->set_fenced(true);
        return grpc::Status::OK;
    }
    template <class Request, class Response>
    void append(const Request& r, Response& p)
    {
        std::lock_guard lock(mutex);
        p.set_batch_id(r.batch_id());
        for(const auto& item: r.items())
        {
            seen.push_back(item);
            auto& counter =
                    counters.try_emplace(item.writer_id(), item.writer_id() == 1 ? 1'000'000'000 : 100).first->second;
            counter = std::max(counter, item.causal_floor().physical_ns()) + 1;
            auto* result = p.add_results();
            result->mutable_id()->set_story_id(r.story_id());
            result->mutable_id()->set_writer_id(item.writer_id());
            result->mutable_id()->set_incarnation(item.incarnation());
            result->mutable_id()->set_sequence(item.sequence());
            result->mutable_assigned_hlc()->set_physical_ns(counter);
            result->set_achieved_durability(r.durability());
        }
    }
    grpc::Status Append(grpc::ServerContext*, const wire::AppendRequest* r, wire::AppendResponse* p) override
    {
        append(*r, *p);
        if(lost > 0)
        {
            --lost;
            return {grpc::StatusCode::UNAVAILABLE, "lost response"};
        }
        return grpc::Status::OK;
    }
    grpc::Status
    AppendStream(grpc::ServerContext*,
                 grpc::ServerReaderWriter<wire::AppendStreamResponse, wire::AppendStreamRequest>* stream) override
    {
        wire::AppendStreamRequest r;
        while(stream->Read(&r))
        {
            wire::AppendStreamResponse p;
            append(r, p);
            if(!stream->Write(p))
                break;
        }
        return grpc::Status::OK;
    }
    size_t keepers;
    std::string endpoint, refuse_identity;
    std::unique_ptr<grpc::Server> server;
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> acquires;
    std::vector<uint64_t> released;
    std::vector<wire::AppendItem> seen;
    std::map<uint64_t, int64_t> counters;
    std::atomic<int64_t> clock_ns{5};
    std::atomic<int> lost{};
};

sdk::AppendSpec inSlice(int64_t slice, std::string payload = "p")
{
    sdk::AppendSpec spec{{"", std::move(payload), "", "", {}}};
    spec.physical = chronolog::TimeReading{slice * kSlice + 7, 1000, chronolog::ClockStatus::Synced};
    return spec;
}

TEST(Lanes, AppendsInAlternatingSlicesLandOnBothLaneWriters)
{
    LaneServer server(2);
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto lanes = client->acquireLanes(1, "w", 2, kSlice);
    ASSERT_TRUE(lanes.ok()) << lanes.status();
    ASSERT_EQ(lanes->lanes(), 2u);
    ASSERT_EQ(server.acquires.size(), 2u);
    EXPECT_EQ(server.acquires[0], std::make_pair(std::string("w/lane0"), std::string("keeper-0")));
    EXPECT_EQ(server.acquires[1], std::make_pair(std::string("w/lane1"), std::string("keeper-1")));
    std::vector<uint64_t> writers;
    for(int64_t slice = 0; slice < 4; ++slice)
    {
        auto result = lanes->append(inSlice(slice));
        ASSERT_TRUE(result.ok()) << result.status();
        writers.push_back(result->event_id.writer_id);
        EXPECT_EQ(result->event_id.sequence, static_cast<uint64_t>(slice / 2 + 1));
    }
    EXPECT_EQ(writers, (std::vector<uint64_t>{1, 2, 1, 2}));
}

TEST(Lanes, BatchSplitsByLaneAndReturnsResultsInInputOrder)
{
    LaneServer server(2);
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto lanes = client->acquireLanes(1, "w", 2, kSlice);
    ASSERT_TRUE(lanes.ok()) << lanes.status();
    const std::vector<sdk::AppendSpec> specs{inSlice(0, "a"),
                                             inSlice(1, "b"),
                                             inSlice(2, "c"),
                                             inSlice(3, "d"),
                                             inSlice(4, "e")};
    auto results = lanes->appendBatch(specs);
    ASSERT_TRUE(results.ok()) << results.status();
    ASSERT_EQ(results->size(), specs.size());
    std::vector<std::pair<uint64_t, uint64_t>> ids;
    for(const auto& result: *results)
    {
        ASSERT_TRUE(result.ok()) << result.status();
        ids.emplace_back(result->event_id.writer_id, result->event_id.sequence);
    }
    EXPECT_EQ(ids, (std::vector<std::pair<uint64_t, uint64_t>>{{1, 1}, {2, 1}, {1, 2}, {2, 2}, {1, 3}}));
    std::map<uint64_t, std::string> payloads;
    for(const auto& item: server.seen) payloads[item.writer_id()] += item.envelope().payload();
    EXPECT_EQ(payloads[1], "ace");
    EXPECT_EQ(payloads[2], "bd");
}

TEST(Lanes, RetriedSpecKeepsItsStampAndLane)
{
    LaneServer server(2);
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto lanes = client->acquireLanes(1, "w", 2, kSlice);
    ASSERT_TRUE(lanes.ok()) << lanes.status();
    sdk::AppendSpec spec{{"", "payload", "", "", {}}};
    server.clock_ns = 5;
    server.lost = 100;
    EXPECT_FALSE(lanes->append(spec).ok());
    server.lost = 0;
    server.clock_ns = kSlice + 5;
    auto retried = lanes->append(spec);
    ASSERT_TRUE(retried.ok()) << retried.status();
    EXPECT_EQ(retried->event_id.writer_id, 1u);
    EXPECT_EQ(retried->event_id.sequence, 1u);
    ASSERT_GE(server.seen.size(), 2u);
    for(const auto& item: server.seen)
    {
        EXPECT_EQ(item.writer_id(), 1u);
        EXPECT_EQ(item.physical().physical_ns(), server.seen.front().physical().physical_ns());
    }
    sdk::AppendSpec other{{"", "other", "", "", {}}};
    other.physical = inSlice(1).physical;
    auto next = lanes->append(other);
    ASSERT_TRUE(next.ok()) << next.status();
    EXPECT_EQ(next->event_id.writer_id, 2u);
}

TEST(Lanes, SequentialWriterHlcsIncreaseAcrossLaneSwitches)
{
    LaneServer server(2);
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto lanes = client->acquireLanes(1, "w", 2, kSlice);
    ASSERT_TRUE(lanes.ok()) << lanes.status();
    std::vector<chronolog::Hlc> hlcs;
    for(const int64_t slice: {0, 1, 0, 1, 1, 0})
    {
        auto result = lanes->append(inSlice(slice));
        ASSERT_TRUE(result.ok()) << result.status();
        hlcs.push_back(result->hlc);
    }
    for(size_t i = 1; i < hlcs.size(); ++i) EXPECT_LT(hlcs[i - 1], hlcs[i]) << i;
}

TEST(Lanes, LaneCountIsCappedByTheRoute)
{
    {
        LaneServer server(2);
        auto client = sdk::Client::Connect(server.options());
        ASSERT_TRUE(client.ok()) << client.status();
        auto lanes = client->acquireLanes(1, "w", 5, kSlice);
        ASSERT_TRUE(lanes.ok()) << lanes.status();
        EXPECT_EQ(lanes->lanes(), 2u);
    }
    LaneServer server(3);
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto lanes = client->acquireLanes(1, "w", 2, kSlice);
    ASSERT_TRUE(lanes.ok()) << lanes.status();
    EXPECT_EQ(lanes->lanes(), 2u);
    EXPECT_EQ(client->acquireLanes(1, "w", 0, kSlice).status().code(), absl::StatusCode::kInvalidArgument);
}

TEST(Lanes, FailedAcquisitionReleasesTheLanesAlreadyAcquired)
{
    LaneServer server(2);
    server.refuse_identity = "w/lane1";
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto lanes = client->acquireLanes(1, "w", 2, kSlice);
    ASSERT_FALSE(lanes.ok());
    EXPECT_EQ(lanes.status().code(), absl::StatusCode::kPermissionDenied);
    EXPECT_EQ(server.released, std::vector<uint64_t>{1});
}
} // namespace
