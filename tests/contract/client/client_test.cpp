#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using namespace std::chrono_literals;

constexpr auto kBusyPause = 10ms;

class Server final
    : public wire::Catalog::Service
    , public wire::Journal::Service
    , public wire::Replay::Service
{
public:
    Server()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Journal::Service*>(this));
        builder.RegisterService(static_cast<wire::Replay::Service*>(this));
        server = builder.BuildAndStart();
        endpoint = "127.0.0.1:" + std::to_string(port);
    }
    ~Server() { server->Shutdown(std::chrono::system_clock::now() + 2s); }
    sdk::ClientOptions options()
    {
        sdk::ClientOptions o;
        o.catalog_endpoint = endpoint;
        o.player_endpoint = endpoint;
        o.rpc_timeout = 2s;
        o.batch_size = 1;
        o.max_in_flight = 2;
        o.retry.backoff = 1ms;
        return o;
    }
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* r, wire::AcquireResponse* p) override
    {
        ++acquisitions;
        p->set_story_id(r->story_id());
        p->set_writer_id(1);
        p->set_incarnation(acquired_incarnation);
        p->mutable_route()->set_epoch(stale ? 1 : 2);
        p->mutable_route()->set_player(endpoint);
        auto* k = p->mutable_route()->add_keepers();
        k->set_process_id("keeper");
        k->set_endpoint(endpoint);
        *p->mutable_assigned_keeper() = *k;
        if(acquisition_route)
        {
            *p->mutable_route() = *acquisition_route;
            *p->mutable_assigned_keeper() = acquisition_route->keepers(0);
        }
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
            auto* result = p.add_results();
            if(refusals > 0)
            {
                --refusals;
                result->mutable_status()->set_code(9);
                result->mutable_status()->set_message("incarnation not yet registered at this keeper");
                auto* route = result->mutable_current_route();
                route->set_epoch(refusal_epoch);
                auto* keeper = route->add_keepers();
                keeper->set_process_id("keeper");
                keeper->set_endpoint(endpoint);
                route->set_player(endpoint);
                continue;
            }
            if(stale && r.epoch() == 1)
            {
                result->mutable_status()->set_code(9);
                auto* route = result->mutable_current_route();
                route->set_epoch(2);
                auto* keeper = route->add_keepers();
                keeper->set_process_id("keeper");
                keeper->set_endpoint(endpoint);
                route->set_player(endpoint);
                if(redirect_route)
                    *route = *redirect_route;
                continue;
            }
            result->mutable_id()->set_story_id(r.story_id());
            result->mutable_id()->set_writer_id(item.writer_id());
            result->mutable_id()->set_incarnation(item.incarnation());
            result->mutable_id()->set_sequence(item.sequence());
            result->mutable_assigned_hlc()->set_physical_ns(100 + static_cast<int64_t>(item.sequence()));
            result->set_achieved_durability(r.durability());
            if(item.envelope().payload() == "invalid")
            {
                result->mutable_status()->set_code(3);
                result->mutable_status()->set_message("invalid trace");
                result->set_achieved_durability(wire::DURABILITY_UNSPECIFIED);
            }
        }
    }
    grpc::Status Append(grpc::ServerContext*, const wire::AppendRequest* r, wire::AppendResponse* p) override
    {
        append(*r, *p);
        if(lose_response.exchange(false))
            return {grpc::StatusCode::UNAVAILABLE, "lost response"};
        return grpc::Status::OK;
    }
    grpc::Status
    AppendStream(grpc::ServerContext*,
                 grpc::ServerReaderWriter<wire::AppendStreamResponse, wire::AppendStreamRequest>* stream) override
    {
        wire::AppendStreamRequest r;
        std::vector<wire::AppendStreamResponse> window;
        while(stream->Read(&r))
        {
            wire::AppendStreamResponse p;
            append(r, p);
            if(lose_response.exchange(false))
                return {grpc::StatusCode::UNAVAILABLE, "lost stream response"};
            if(!reorder)
            {
                if(!stream->Write(p))
                    break;
            }
            else
            {
                window.push_back(std::move(p));
                if(window.size() == 2)
                {
                    stream->Write(window[1]);
                    stream->Write(window[0]);
                    window.clear();
                }
            }
        }
        for(const auto& p: window) stream->Write(p);
        return grpc::Status::OK;
    }
    template <class Response>
    static Response eventResponse(uint64_t sequence)
    {
        Response p;
        auto* e = p.mutable_batch()->add_events();
        e->mutable_id()->set_story_id(1);
        e->mutable_id()->set_writer_id(1);
        e->mutable_id()->set_incarnation(1);
        e->mutable_id()->set_sequence(sequence);
        e->mutable_hlc()->set_physical_ns(1000 + static_cast<int64_t>(sequence));
        return p;
    }
    grpc::Status Read(grpc::ServerContext* context,
                      const wire::ReadRequest*,
                      grpc::ServerWriter<wire::ReadResponse>* stream) override
    {
        if(busy_events)
        {
            for(uint64_t sequence = 1; sequence <= busy_events; ++sequence)
            {
                if(context->IsCancelled())
                    return {grpc::StatusCode::CANCELLED, "cancelled"};
                std::this_thread::sleep_for(kBusyPause);
                stream->Write(eventResponse<wire::ReadResponse>(sequence));
            }
            wire::ReadResponse end;
            end.mutable_completion()->set_complete(true);
            stream->Write(end);
            return grpc::Status::OK;
        }
        wire::ReadResponse p;
        p.mutable_completion()->set_reason(wire::INCOMPLETE_REASON_TRUNCATED);
        p.mutable_completion()->mutable_frontier()->set_physical_ns(500);
        stream->Write(p);
        return grpc::Status::OK;
    }
    grpc::Status Tail(grpc::ServerContext* context,
                      const wire::TailRequest* r,
                      grpc::ServerWriter<wire::TailResponse>* stream) override
    {
        const int call = ++tails;
        {
            std::lock_guard lock(mutex);
            resumes.push_back(r->from());
        }
        if(refuse_tail)
            return {grpc::StatusCode::UNAVAILABLE, "refused"};
        if(busy_events)
        {
            for(uint64_t sequence = r->from().id().sequence() + 1; sequence <= busy_events; ++sequence)
            {
                if(context->IsCancelled())
                    return {grpc::StatusCode::CANCELLED, "cancelled"};
                std::this_thread::sleep_for(kBusyPause);
                stream->Write(eventResponse<wire::TailResponse>(sequence));
            }
            wire::TailResponse end;
            end.mutable_completion();
            stream->Write(end);
            return grpc::Status::OK;
        }
        if(block_tail)
        {
            while(!context->IsCancelled()) std::this_thread::sleep_for(1ms);
            return {grpc::StatusCode::CANCELLED, "cancelled"};
        }
        wire::TailResponse p;
        auto* e = p.mutable_batch()->add_events();
        e->mutable_id()->set_story_id(1);
        e->mutable_id()->set_writer_id(1);
        e->mutable_id()->set_incarnation(1);
        e->mutable_id()->set_sequence(static_cast<uint64_t>(call));
        e->mutable_hlc()->set_physical_ns(1000 + call);
        stream->Write(p);
        if(call <= flaky_calls)
            return {grpc::StatusCode::UNAVAILABLE, "disconnected"};
        p.Clear();
        p.mutable_completion();
        stream->Write(p);
        return grpc::Status::OK;
    }
    std::string endpoint;
    std::unique_ptr<grpc::Server> server;
    std::mutex mutex;
    std::vector<wire::AppendItem> seen;
    std::vector<wire::Position> resumes;
    std::atomic<int> tails{}, acquisitions{};
    std::atomic<bool> lose_response{};
    bool stale{}, reorder{}, block_tail{}, refuse_tail{};
    int flaky_calls{1};
    int refusals{};
    uint64_t refusal_epoch{2};
    uint64_t acquired_incarnation{1};
    std::optional<wire::Route> redirect_route, acquisition_route;
    uint64_t busy_events{};
};

TEST(ClientContract, ClientRetriesAnAppendRefusedBeforeTheKeeperLearnsTheAcquisition)
{
    for(bool streaming: {false, true})
    {
        Server server;
        server.refusals = 2;
        auto client = sdk::Client::Connect(server.options());
        ASSERT_TRUE(client.ok()) << client.status();
        auto writer = client->acquire(1, "writer");
        ASSERT_TRUE(writer.ok()) << writer.status();
        sdk::AppendSpec spec{{"", "payload", "", "", {}}};
        auto result = streaming ? writer->appendBatch(std::span(&spec, 1)) : [&]() -> absl::StatusOr<sdk::BatchResult>
        {
            auto one = writer->append(spec);
            if(!one.ok())
                return one.status();
            return sdk::BatchResult{*one};
        }();
        ASSERT_TRUE(result.ok()) << result.status();
        ASSERT_EQ(result->size(), 1u);
        ASSERT_TRUE(result->front().ok()) << result->front().status();
        EXPECT_TRUE(result->front()->acked());
        EXPECT_EQ(result->front()->event_id.writer_id, writer->acquisition().writer_id);
        EXPECT_EQ(result->front()->event_id.incarnation, writer->acquisition().incarnation);
        EXPECT_EQ(result->front()->event_id.sequence, 1u);
        ASSERT_EQ(server.seen.size(), 3u);
        for(const auto& item: server.seen) EXPECT_EQ(item.SerializeAsString(), server.seen.front().SerializeAsString());
    }
}
TEST(ClientContract, ClientEndsAPersistentSameEpochRefusalFailedPrecondition)
{
    Server server;
    server.refusals = 100;
    auto options = server.options();
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok()) << writer.status();
    auto result = writer->append({{"", "payload", "", "", {}}});
    EXPECT_EQ(result.status().code(), absl::StatusCode::kFailedPrecondition) << result.status();
    EXPECT_EQ(result.status().message(), "incarnation not yet registered at this keeper");
    ASSERT_EQ(server.seen.size(), options.retry.max_retries + 1);
    for(const auto& item: server.seen) EXPECT_EQ(item.SerializeAsString(), server.seen.front().SerializeAsString());
}
TEST(ClientContract, ClientRetriesAnAppendWhileTheKeeperEpochLags)
{
    Server server;
    server.refusals = 2;
    server.refusal_epoch = 1;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok()) << writer.status();
    auto result = writer->append({{"", "payload", "", "", {}}});
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->acked());
    EXPECT_EQ(writer->acquisition().route.epoch, 2u);
    ASSERT_EQ(server.seen.size(), 3u);
    for(const auto& item: server.seen) EXPECT_EQ(item.SerializeAsString(), server.seen.front().SerializeAsString());
}
TEST(ClientContract, ClientKeepsItsSurvivingKeeperAfterAnEpochChange)
{
    Server server, joined;
    server.stale = true;
    wire::Route route;
    route.set_epoch(2);
    route.set_player(server.endpoint);
    auto* survivor = route.add_keepers();
    survivor->set_process_id("keeper");
    survivor->set_endpoint(server.endpoint);
    auto* newcomer = route.add_keepers();
    newcomer->set_process_id("joined");
    newcomer->set_endpoint(joined.endpoint);
    server.redirect_route = route;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok()) << writer.status();
    auto result = writer->append({{"", "payload", "", "", {}}});
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->acked());
    EXPECT_EQ(writer->acquisition().assigned_keeper.process_id, "keeper");
    EXPECT_EQ(writer->acquisition().route.epoch, 2u);
    ASSERT_EQ(server.seen.size(), 2u);
    EXPECT_EQ(server.seen[0].SerializeAsString(), server.seen[1].SerializeAsString());
    EXPECT_TRUE(joined.seen.empty());
    EXPECT_EQ(server.acquisitions.load(), 1);
}
TEST(ClientContract, ClientWhoseKeeperWasRemovedReportsOutcomeUnknownAndReacquires)
{
    Server server, replacement;
    server.stale = true;
    wire::Route route;
    route.set_epoch(2);
    route.set_player(server.endpoint);
    auto* keeper = route.add_keepers();
    keeper->set_process_id("replacement");
    keeper->set_endpoint(replacement.endpoint);
    server.redirect_route = route;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok()) << writer.status();
    sdk::AppendSpec spec{{"", "payload", "", "", {}}};
    auto result = writer->append(spec);
    EXPECT_EQ(result.status().code(), absl::StatusCode::kUnknown) << result.status();
    EXPECT_NE(result.status().message().find("outcome unknown"), std::string::npos);
    EXPECT_NE(result.status().message().find("re-acquire"), std::string::npos);
    EXPECT_TRUE(replacement.seen.empty());
    auto again = writer->append(spec);
    EXPECT_EQ(again.status().code(), absl::StatusCode::kFailedPrecondition) << again.status();
    EXPECT_NE(again.status().message().find("re-acquire"), std::string::npos);
    ASSERT_EQ(server.seen.size(), 1u);
    EXPECT_EQ(server.acquisitions.load(), 1);
    server.acquisition_route = route;
    server.acquired_incarnation = 2;
    auto fresh = client->acquire(1, "writer");
    ASSERT_TRUE(fresh.ok()) << fresh.status();
    auto appended = fresh->append(spec);
    ASSERT_TRUE(appended.ok()) << appended.status();
    EXPECT_TRUE(appended->acked());
    EXPECT_EQ(appended->event_id.incarnation, 2u);
    EXPECT_EQ(appended->event_id.sequence, 1u);
    ASSERT_EQ(replacement.seen.size(), 1u);
}
TEST(ClientContract, ClientRetriesOnStaleEpochWithSameEventIds)
{
    Server server;
    server.stale = true;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok()) << client.status();
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok());
    auto result = writer->append({{"", "payload", "", "", {}}});
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_TRUE(result->acked());
    EXPECT_EQ(writer->acquisition().route.epoch, 2u);
    ASSERT_EQ(server.seen.size(), 2u);
    EXPECT_EQ(server.seen[0].SerializeAsString(), server.seen[1].SerializeAsString());
    EXPECT_EQ(result->event_id.sequence, 1u);
}
TEST(ClientContract, TransportRetryPreservesIdentityAndPhysicalReading)
{
    Server server;
    server.lose_response = true;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok());
    auto result = writer->append({{"", "payload", "", "", {}}});
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(server.seen.size(), 2u);
    EXPECT_EQ(server.seen[0].sequence(), server.seen[1].sequence());
    EXPECT_EQ(server.seen[0].physical().SerializeAsString(), server.seen[1].physical().SerializeAsString());
    EXPECT_EQ(server.seen[1].causal_floor().physical_ns(), 0);
    auto second = writer->append({{"", "next", "", "", {}}, chronolog::Durability::Accepted});
    ASSERT_TRUE(second.ok());
    EXPECT_FALSE(second->acked());
    EXPECT_EQ(second->event_id.sequence, 2u);
    EXPECT_EQ(server.seen.back().causal_floor().physical_ns(), result->hlc.physical_ns);
}
TEST(ClientContract, AppendStreamCorrelatesReorderedBatches)
{
    Server server;
    server.reorder = true;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok());
    std::vector<sdk::AppendSpec> specs(6);
    for(size_t i = 0; i < specs.size(); ++i) specs[i].envelope.payload = std::to_string(i);
    auto results = writer->appendBatch(specs);
    ASSERT_TRUE(results.ok()) << results.status();
    ASSERT_EQ(results->size(), specs.size());
    for(size_t i = 0; i < results->size(); ++i)
    {
        ASSERT_TRUE((*results)[i].ok());
        EXPECT_EQ((*results)[i]->event_id.sequence, i + 1);
    }
}
TEST(ClientContract, ItemFailureDoesNotConsumeSequence)
{
    Server server;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok());
    auto invalid = writer->append({{"", "invalid", "", "", {}}});
    ASSERT_FALSE(invalid.ok());
    EXPECT_EQ(invalid.status().code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(invalid.status().message(), "invalid trace");
    auto valid = writer->append({{"", "valid", "", "", {}}});
    ASSERT_TRUE(valid.ok());
    EXPECT_EQ(valid->event_id.sequence, 1u);
}
TEST(ClientContract, ClientResumesTailAfterStreamError)
{
    Server server;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto tail = client->tail(1);
    ASSERT_TRUE(tail.ok());
    auto first = tail->next();
    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_TRUE(*first);
    auto second = tail->next();
    ASSERT_TRUE(second.ok()) << second.status();
    ASSERT_TRUE(*second);
    ASSERT_EQ((**first).events.size(), 1u);
    ASSERT_EQ((**second).events.size(), 1u);
    EXPECT_EQ((**second).events[0].id.sequence, 2u);
    ASSERT_EQ(server.resumes.size(), 2u);
    EXPECT_EQ(server.resumes[1].id().sequence(), (**first).events[0].id.sequence);
    EXPECT_EQ(server.resumes[1].hlc().physical_ns(), (**first).events[0].hlc.physical_ns);
    auto completion = tail->next();
    ASSERT_TRUE(completion.ok());
    ASSERT_TRUE(*completion);
    EXPECT_TRUE((**completion).completion.has_value());
    auto end = tail->next();
    ASSERT_TRUE(end.ok());
    EXPECT_FALSE(*end);
    EXPECT_EQ(server.acquisitions.load(), 0);
    auto writer = client->acquire(1, "causal-writer");
    ASSERT_TRUE(writer.ok());
    auto written = writer->append({{"", "causal", "", "", {}}});
    ASSERT_TRUE(written.ok());
    EXPECT_EQ(server.seen.back().causal_floor().physical_ns(), (**second).events[0].hlc.physical_ns);
}
TEST(ClientContract, CancelUnblocksPullAndShortDeadlineIsHonored)
{
    Server server;
    server.block_tail = true;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto tail = client->tail(1);
    ASSERT_TRUE(tail.ok());
    auto pulling = std::async(std::launch::async, [&] { return tail->next(); });
    for(int i = 0; i < 1000 && !server.tails.load(); ++i) std::this_thread::sleep_for(1ms);
    tail->cancel();
    tail->cancel();
    ASSERT_EQ(pulling.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(pulling.get().status().code(), absl::StatusCode::kCancelled);
    auto short_tail = client->tail(1);
    ASSERT_TRUE(short_tail.ok());
    const auto begin = std::chrono::steady_clock::now();
    auto timed = short_tail->next(std::chrono::system_clock::now() + 30ms);
    EXPECT_EQ(timed.status().code(), absl::StatusCode::kDeadlineExceeded);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s);
}
TEST(ClientContract, TruncatedReadExposesContinuationWithoutAcquiring)
{
    Server server;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto read = client->read(1, {{0, 0}, {1000, 0}});
    ASSERT_TRUE(read.ok());
    auto item = read->next();
    ASSERT_TRUE(item.ok()) << item.status();
    ASSERT_TRUE(*item);
    ASSERT_TRUE((**item).continuation);
    EXPECT_EQ((**item).continuation->physical_ns, 500);
    EXPECT_EQ(server.acquisitions.load(), 0);
}
TEST(ClientContract, ClientTailSurvivesLongerThanRpcTimeout)
{
    constexpr uint64_t events = 250;
    constexpr auto rpc_timeout = 400ms;
    static_assert(events * kBusyPause > 4 * rpc_timeout);
    Server server;
    server.busy_events = events;
    auto options = server.options();
    options.rpc_timeout = rpc_timeout;
    options.retry.max_retries = 3;
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok()) << client.status();
    auto tail = client->tail(1);
    ASSERT_TRUE(tail.ok());
    std::vector<uint64_t> sequences;
    for(bool completed = false; !completed;)
    {
        auto item = tail->next();
        ASSERT_TRUE(item.ok()) << "after " << sequences.size() << " events: " << item.status();
        ASSERT_TRUE(*item);
        for(const auto& event: (**item).events) sequences.push_back(event.id.sequence);
        completed = (**item).completion.has_value();
    }
    ASSERT_EQ(sequences.size(), events);
    for(size_t i = 0; i < sequences.size(); ++i) EXPECT_EQ(sequences[i], i + 1);
    EXPECT_EQ(server.tails.load(), 1);
}
TEST(ClientContract, ClientReadSurvivesLongerThanRpcTimeout)
{
    constexpr uint64_t events = 250;
    constexpr auto rpc_timeout = 400ms;
    static_assert(events * kBusyPause > 4 * rpc_timeout);
    Server server;
    server.busy_events = events;
    auto options = server.options();
    options.rpc_timeout = rpc_timeout;
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok()) << client.status();
    auto read = client->read(1, {{0, 0}, {1000000, 0}});
    ASSERT_TRUE(read.ok());
    std::vector<uint64_t> sequences;
    for(bool completed = false; !completed;)
    {
        auto item = read->next();
        ASSERT_TRUE(item.ok()) << "after " << sequences.size() << " events: " << item.status();
        ASSERT_TRUE(*item);
        for(const auto& event: (**item).events) sequences.push_back(event.id.sequence);
        completed = (**item).completion.has_value();
    }
    ASSERT_EQ(sequences.size(), events);
    for(size_t i = 0; i < sequences.size(); ++i) EXPECT_EQ(sequences[i], i + 1);
}
TEST(ClientContract, ClientTailRetryCounterResetsOnDelivery)
{
    Server server;
    server.flaky_calls = 6;
    auto options = server.options();
    options.retry.max_retries = 3;
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok());
    auto tail = client->tail(1);
    ASSERT_TRUE(tail.ok());
    for(uint64_t expected = 1; expected <= 7; ++expected)
    {
        auto item = tail->next();
        ASSERT_TRUE(item.ok()) << "event " << expected << ": " << item.status();
        ASSERT_TRUE(*item);
        ASSERT_EQ((**item).events.size(), 1u);
        EXPECT_EQ((**item).events[0].id.sequence, expected);
    }
    auto completion = tail->next();
    ASSERT_TRUE(completion.ok()) << completion.status();
    ASSERT_TRUE(*completion);
    EXPECT_TRUE((**completion).completion.has_value());
    EXPECT_EQ(server.tails.load(), 7);
    ASSERT_EQ(server.resumes.size(), 7u);
    for(size_t i = 1; i < server.resumes.size(); ++i) EXPECT_EQ(server.resumes[i].id().sequence(), i);
}
TEST(ClientContract, ClientTailGivesUpAfterMaxConsecutiveFailures)
{
    Server server;
    server.refuse_tail = true;
    auto options = server.options();
    options.retry.max_retries = 3;
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok());
    auto tail = client->tail(1);
    ASSERT_TRUE(tail.ok());
    auto item = tail->next();
    EXPECT_EQ(item.status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(server.tails.load(), 4);
}
TEST(ClientContract, CancelDuringTailBackoffReturnsCancelled)
{
    Server server;
    server.refuse_tail = true;
    auto options = server.options();
    options.rpc_timeout = 30s;
    options.retry.backoff = 15s;
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok());
    auto tail = client->tail(1);
    ASSERT_TRUE(tail.ok());
    auto pulling = std::async(std::launch::async, [&] { return tail->next(); });
    for(int i = 0; i < 5000 && !server.tails.load(); ++i) std::this_thread::sleep_for(1ms);
    ASSERT_EQ(server.tails.load(), 1);
    std::this_thread::sleep_for(100ms);
    tail->cancel();
    ASSERT_EQ(pulling.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(pulling.get().status().code(), absl::StatusCode::kCancelled);
    EXPECT_EQ(server.tails.load(), 1);
}
} // namespace

TEST(ClientContract, WriterSerializesConcurrentAppendSequences)
{
    Server server;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok());
    std::atomic<bool> ok{true};
    std::vector<std::jthread> callers;
    for(int thread = 0; thread < 4; ++thread)
        callers.emplace_back(
                [&]
                {
                    for(int i = 0; i < 8; ++i)
                        if(!writer->append({{"", "concurrent", "", "", {}}}).ok())
                            ok = false;
                });
    callers.clear();
    EXPECT_TRUE(ok.load());
    ASSERT_EQ(server.seen.size(), 32u);
    for(size_t i = 0; i < server.seen.size(); ++i)
    {
        EXPECT_EQ(server.seen[i].sequence(), i + 1);
        if(i)
        {
            EXPECT_GT(server.seen[i].physical().physical_ns(), server.seen[i - 1].physical().physical_ns());
        }
    }
}

TEST(ClientContract, AppendStreamRetriesUnknownItemsWithOriginalIdentities)
{
    Server server;
    server.stale = true;
    server.lose_response = true;
    auto client = sdk::Client::Connect(server.options());
    ASSERT_TRUE(client.ok());
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok());
    std::vector<sdk::AppendSpec> specs(4);
    auto results = writer->appendBatch(specs);
    ASSERT_TRUE(results.ok()) << results.status();
    for(size_t i = 0; i < results->size(); ++i)
    {
        ASSERT_TRUE((*results)[i].ok());
        EXPECT_EQ((*results)[i]->event_id.sequence, i + 1);
    }
    std::map<uint64_t, std::string> physical;
    for(const auto& item: server.seen)
    {
        auto [it, inserted] = physical.emplace(item.sequence(), item.physical().SerializeAsString());
        if(!inserted)
        {
            EXPECT_EQ(it->second, item.physical().SerializeAsString());
        }
    }
    EXPECT_EQ(writer->acquisition().route.epoch, 2u);
}
