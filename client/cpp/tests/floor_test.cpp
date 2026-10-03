#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <barrier>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using chronolog::Hlc;
using namespace std::chrono_literals;

class FloorPeer final
    : public wire::Catalog::Service
    , public wire::Journal::Service
    , public wire::Replay::Service
{
public:
    FloorPeer()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Journal::Service*>(this));
        builder.RegisterService(static_cast<wire::Replay::Service*>(this));
        server_ = builder.BuildAndStart();
        endpoint_ = "127.0.0.1:" + std::to_string(port);
    }
    ~FloorPeer() override
    {
        server_->Shutdown(std::chrono::system_clock::now() + 2s);
        server_->Wait();
    }
    sdk::ClientOptions options() const
    {
        sdk::ClientOptions options;
        options.catalog_endpoint = endpoint_;
        options.player_endpoint = endpoint_;
        options.batch_size = 1;
        return options;
    }
    std::vector<Hlc> floors()
    {
        std::lock_guard lock(mutex_);
        return floors_;
    }
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* r, wire::AcquireResponse* p) override
    {
        p->set_story_id(r->story_id());
        p->set_writer_id(1);
        p->set_incarnation(1);
        p->mutable_route()->set_epoch(1);
        auto* keeper = p->mutable_route()->add_keepers();
        keeper->set_process_id("keeper");
        keeper->set_endpoint(endpoint_);
        *p->mutable_assigned_keeper() = *keeper;
        return grpc::Status::OK;
    }
    template <class Request, class Response>
    void append(const Request& request, Response& response)
    {
        std::lock_guard lock(mutex_);
        response.set_batch_id(request.batch_id());
        for(const auto& item: request.items())
        {
            const Hlc floor{item.causal_floor().physical_ns(), item.causal_floor().logical()};
            floors_.push_back(floor);
            auto* result = response.add_results();
            if(floor.physical_ns == std::numeric_limits<int64_t>::max())
            {
                result->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kInvalidArgument));
                result->mutable_status()->set_message("keeper causal floor skew limit");
                continue;
            }
            result->mutable_id()->set_story_id(request.story_id());
            result->mutable_id()->set_writer_id(item.writer_id());
            result->mutable_id()->set_incarnation(item.incarnation());
            result->mutable_id()->set_sequence(item.sequence());
            result->mutable_assigned_hlc()->set_physical_ns(floor.physical_ns + 1);
            result->set_achieved_durability(request.durability());
        }
    }
    grpc::Status Append(grpc::ServerContext*, const wire::AppendRequest* r, wire::AppendResponse* p) override
    {
        append(*r, *p);
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
            append(request, response);
            if(!stream->Write(response))
                break;
        }
        return grpc::Status::OK;
    }
    template <class Response>
    static Response event(uint64_t story, Hlc hlc)
    {
        Response response;
        auto* event = response.mutable_batch()->add_events();
        event->mutable_id()->set_story_id(story);
        event->mutable_id()->set_writer_id(1);
        event->mutable_id()->set_incarnation(1);
        event->mutable_id()->set_sequence(1);
        event->mutable_hlc()->set_physical_ns(hlc.physical_ns);
        event->mutable_hlc()->set_logical(hlc.logical);
        return response;
    }
    grpc::Status Read(grpc::ServerContext*,
                      const wire::ReadRequest* request,
                      grpc::ServerWriter<wire::ReadResponse>* stream) override
    {
        stream->Write(
                event<wire::ReadResponse>(request->story_id(), request->has_physical() ? Hlc{150, 1} : Hlc{100, 2}));
        wire::ReadResponse response;
        response.mutable_completion()->set_complete(true);
        response.mutable_completion()->mutable_frontier()->set_physical_ns(1000);
        stream->Write(response);
        return grpc::Status::OK;
    }
    grpc::Status Tail(grpc::ServerContext*,
                      const wire::TailRequest* request,
                      grpc::ServerWriter<wire::TailResponse>* stream) override
    {
        stream->Write(event<wire::TailResponse>(request->story_id(), {200, 3}));
        wire::TailResponse response;
        response.mutable_completion();
        stream->Write(response);
        return grpc::Status::OK;
    }

private:
    std::string endpoint_;
    std::unique_ptr<grpc::Server> server_;
    std::mutex mutex_;
    std::vector<Hlc> floors_;
};

TEST(ClientFloor, ObserveFloorNeverLowersTheFloor)
{
    FloorPeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    const sdk::Client& view = *client;
    EXPECT_EQ(view.causalFloor(), (Hlc{}));
    const std::vector<std::pair<Hlc, Hlc>> observations{{{10, 7}, {10, 7}},
                                                        {{10, 3}, {10, 7}},
                                                        {{9, 99}, {10, 7}},
                                                        {{10, 7}, {10, 7}},
                                                        {{10, 8}, {10, 8}},
                                                        {{11, 0}, {11, 0}},
                                                        {{-1, 0}, {11, 0}}};
    for(const auto& [floor, expected]: observations)
    {
        client->observeFloor(floor);
        EXPECT_EQ(view.causalFloor(), expected);
    }
    const Hlc largest{std::numeric_limits<int64_t>::max(), std::numeric_limits<uint32_t>::max()};
    client->observeFloor(largest);
    client->observeFloor({});
    EXPECT_EQ(view.causalFloor(), largest);
    EXPECT_TRUE(peer.floors().empty());
}

TEST(ClientFloor, AppendCarriesTheObservedFloor)
{
    FloorPeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto first = client->acquire(1, "first");
    auto second = client->acquire(2, "second");
    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_TRUE(second.ok()) << second.status();
    const Hlc unary_floor{42, 7};
    client->observeFloor(unary_floor);
    auto result = first->append({{"", "unary", "", "", {}}});
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(peer.floors().size(), 1u);
    EXPECT_EQ(peer.floors().front(), unary_floor);
    EXPECT_GT(result->hlc, unary_floor);
    const Hlc stream_floor{84, std::numeric_limits<uint32_t>::max()};
    client->observeFloor(stream_floor);
    std::vector<sdk::AppendSpec> specs(2, sdk::AppendSpec{{"", "stream", "", "", {}}});
    auto batch = second->appendBatch(specs);
    ASSERT_TRUE(batch.ok()) << batch.status();
    ASSERT_EQ(batch->size(), 2u);
    for(const auto& item: *batch)
    {
        ASSERT_TRUE(item.ok()) << item.status();
        EXPECT_GT(item->hlc, stream_floor);
    }
    auto floors = peer.floors();
    ASSERT_EQ(floors.size(), 3u);
    EXPECT_GE(floors[1], stream_floor);
    EXPECT_GE(floors[2], stream_floor);
    const Hlc future{std::numeric_limits<int64_t>::max(), 0};
    client->observeFloor(future);
    auto rejected = first->append({{"", "keeper decides", "", "", {}}});
    EXPECT_EQ(rejected.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(rejected.status().message(), "keeper causal floor skew limit");
    floors = peer.floors();
    ASSERT_EQ(floors.size(), 4u);
    EXPECT_EQ(floors.back(), future);
}

TEST(ClientFloor, DeliveredEventsKeepRaisingTheFloorAfterObserve)
{
    FloorPeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    client->observeFloor({50, 9});
    auto read = client->read(1, {{0, 0}, {1000, 0}});
    ASSERT_TRUE(read.ok()) << read.status();
    auto item = read->next();
    ASSERT_TRUE(item.ok()) << item.status();
    ASSERT_TRUE(*item);
    ASSERT_EQ((**item).events.size(), 1u);
    EXPECT_EQ(client->causalFloor(), (Hlc{100, 2}));
    auto completion = read->next();
    ASSERT_TRUE(completion.ok()) << completion.status();
    ASSERT_TRUE(*completion);
    ASSERT_TRUE((**completion).completion);
    EXPECT_EQ(client->causalFloor(), (Hlc{100, 2}));
    auto physical = client->readPhysical(1, {0, 1000});
    ASSERT_TRUE(physical.ok()) << physical.status();
    item = physical->next();
    ASSERT_TRUE(item.ok()) << item.status();
    ASSERT_TRUE(*item);
    ASSERT_EQ((**item).events.size(), 1u);
    EXPECT_EQ(client->causalFloor(), (Hlc{150, 1}));
    auto tail = client->tail(2);
    ASSERT_TRUE(tail.ok()) << tail.status();
    item = tail->next();
    ASSERT_TRUE(item.ok()) << item.status();
    ASSERT_TRUE(*item);
    ASSERT_EQ((**item).events.size(), 1u);
    EXPECT_EQ(client->causalFloor(), (Hlc{200, 3}));
    auto writer = client->acquire(3, "after delivery");
    ASSERT_TRUE(writer.ok()) << writer.status();
    auto receipt = writer->append({{"", "receipt", "", "", {}}});
    ASSERT_TRUE(receipt.ok()) << receipt.status();
    EXPECT_GT(receipt->hlc, (Hlc{200, 3}));
    EXPECT_EQ(client->causalFloor(), receipt->hlc);
    client->observeFloor({});
    EXPECT_EQ(client->causalFloor(), receipt->hlc);
}

TEST(ClientFloor, ConcurrentObserveAndAppendIsRaceFree)
{
    FloorPeer peer;
    auto client = sdk::Client::Connect(peer.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto unary = client->acquire(1, "unary");
    auto streaming = client->acquire(2, "streaming");
    ASSERT_TRUE(unary.ok()) << unary.status();
    ASSERT_TRUE(streaming.ok()) << streaming.status();
    constexpr int rounds = 32;
    const Hlc initial{10, 7};
    client->observeFloor(initial);
    std::barrier start(4);
    std::vector<std::jthread> threads;
    for(int observer = 0; observer < 2; ++observer)
        threads.emplace_back(
                [&, observer]
                {
                    Hlc previous = initial;
                    for(int i = 0; i < rounds; ++i)
                    {
                        start.arrive_and_wait();
                        const Hlc floor{1000 + i, static_cast<uint32_t>(observer)};
                        client->observeFloor(floor);
                        const auto current = client->causalFloor();
                        EXPECT_GE(current, floor);
                        EXPECT_GE(current, previous);
                        previous = current;
                        start.arrive_and_wait();
                    }
                });
    for(bool batch: {false, true})
        threads.emplace_back(
                [&, batch]
                {
                    for(int i = 0; i < rounds; ++i)
                    {
                        start.arrive_and_wait();
                        auto& writer = batch ? *streaming : *unary;
                        sdk::AppendSpec spec{{"", "concurrent", "", "", {}}};
                        if(batch)
                        {
                            auto result = writer.appendBatch(std::span(&spec, 1));
                            EXPECT_TRUE(result.ok()) << result.status();
                            if(result.ok())
                            {
                                EXPECT_EQ(result->size(), 1u);
                                if(result->size() == 1)
                                {
                                    EXPECT_TRUE(result->front().ok()) << result->front().status();
                                    if(result->front().ok())
                                    {
                                        EXPECT_GE(client->causalFloor(), result->front()->hlc);
                                    }
                                }
                            }
                        }
                        else
                        {
                            auto result = writer.append(spec);
                            EXPECT_TRUE(result.ok()) << result.status();
                            if(result.ok())
                            {
                                EXPECT_GE(client->causalFloor(), result->hlc);
                            }
                        }
                        start.arrive_and_wait();
                    }
                });
    threads.clear();
    const auto floors = peer.floors();
    ASSERT_EQ(floors.size(), 2u * rounds);
    for(Hlc floor: floors) EXPECT_GE(floor, initial);
    EXPECT_GE(client->causalFloor(), (Hlc{1000 + rounds - 1, 1}));
}
} // namespace
