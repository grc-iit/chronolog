// Port of the half-open hop tests in src/chrono-common/rpc/channel_pool_test.cpp to the SDK's own channel policy
// (M11.3): a hop that stops forwarding without closing must be noticed by an unanswered ping, after which the
// SDK reaches the peer that took over behind the hop.
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "chronolog/client/client.h"
#include "../lib/catalog_target.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using namespace std::chrono_literals;

class Peer final
    : public wire::Catalog::Service
    , public wire::Replay::Service
    , public wire::Journal::Service
{
public:
    explicit Peer(std::string name, std::string keeper = {})
        : name_(std::move(name))
        , keeper_(std::move(keeper))
    {
        grpc::ServerBuilder builder;
        // The server policy of every ChronoLog service, so the client's keepalive pings are not strikes.
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 500);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(static_cast<wire::Catalog::Service*>(this));
        builder.RegisterService(static_cast<wire::Replay::Service*>(this));
        builder.RegisterService(static_cast<wire::Journal::Service*>(this));
        server_ = builder.BuildAndStart();
    }
    ~Peer() override { stop(); }
    void stop()
    {
        if(server_)
        {
            server_->Shutdown(std::chrono::system_clock::now() + 2s);
            server_->Wait();
            server_.reset();
        }
    }
    grpc::Status
    ListChronicles(grpc::ServerContext*, const wire::ListChroniclesRequest*, wire::ListChroniclesResponse* p) override
    {
        p->add_chronicles()->set_name(name_);
        return grpc::Status::OK;
    }
    grpc::Status Acquire(grpc::ServerContext*, const wire::AcquireRequest* r, wire::AcquireResponse* p) override
    {
        p->set_story_id(r->story_id());
        p->set_writer_id(1);
        p->set_incarnation(1);
        p->mutable_route()->set_epoch(1);
        p->mutable_assigned_keeper()->set_process_id("keeper");
        p->mutable_assigned_keeper()->set_endpoint(keeper_.empty() ? "127.0.0.1:" + std::to_string(port) : keeper_);
        return grpc::Status::OK;
    }
    grpc::Status GetStory(grpc::ServerContext*, const wire::GetStoryRequest* r, wire::GetStoryResponse* p) override
    {
        auto* story = p->mutable_story();
        story->set_story_id(r->story_id());
        story->set_epoch(1);
        story->mutable_route()->set_epoch(1);
        story->mutable_route()->set_player("127.0.0.1:" + std::to_string(port));
        return grpc::Status::OK;
    }
    grpc::Status
    Read(grpc::ServerContext*, const wire::ReadRequest*, grpc::ServerWriter<wire::ReadResponse>* writer) override
    {
        wire::ReadResponse response;
        response.mutable_completion()->set_complete(true);
        writer->Write(response);
        return grpc::Status::OK;
    }
    grpc::Status
    Tail(grpc::ServerContext*, const wire::TailRequest* r, grpc::ServerWriter<wire::TailResponse>* writer) override
    {
        wire::TailResponse response;
        auto* event = response.mutable_batch()->add_events();
        event->mutable_id()->set_story_id(r->story_id());
        event->mutable_id()->set_sequence(1);
        event->mutable_hlc()->set_physical_ns(5);
        writer->Write(response);
        response.Clear();
        response.mutable_completion()->set_complete(false);
        writer->Write(response);
        return grpc::Status::OK;
    }
    template <class Request, class Response>
    void append(const Request& r, Response& p)
    {
        p.set_batch_id(r.batch_id());
        for(const auto& item: r.items())
        {
            auto* result = p.add_results();
            result->mutable_id()->set_story_id(r.story_id());
            result->mutable_id()->set_writer_id(1);
            result->mutable_id()->set_incarnation(1);
            result->mutable_id()->set_sequence(item.sequence());
            result->mutable_assigned_hlc()->set_physical_ns(5);
            result->set_achieved_durability(wire::DURABILITY_DURABLE);
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
            stream->Write(response);
        }
        return grpc::Status::OK;
    }
    int port = 0;

private:
    std::string name_;
    const std::string keeper_;
    std::unique_ptr<grpc::Server> server_;
};

// A TCP hop in front of one peer. goDark() leaves every connection open and stops forwarding on the ones that exist,
// the way a dropped route or a dead switch looks to both ends; connections made afterwards reach the successor.
class Hop
{
public:
    explicit Hop(int upstream)
        : upstream_(upstream)
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        EXPECT_EQ(::listen(listener_, 16), 0);
        socklen_t length = sizeof(address);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        acceptor_ = std::thread([this] { accept(); });
    }
    ~Hop()
    {
        stop_ = true;
        bytes_cv_.notify_all();
        acceptor_.join();
        for(auto& pump: pumps_) pump.join();
        for(int fd: sockets_) ::close(fd);
        ::close(listener_);
    }
    int port() const { return port_; }
    void goDark(int successor)
    {
        upstream_ = successor;
        ++generation_;
    }
    // Withhold the server's first bytes for 500 ms across reconnects, exceeding the 100 ms connect timeout.
    void delayHandshakes()
    {
        std::lock_guard lock(sockets_mutex_);
        slow_handshake_ = true;
        release_bytes_.reset();
        for(int fd: sockets_) ::shutdown(fd, SHUT_RDWR);
    }
    int delayedHandshakes() const { return delayed_handshakes_; }

private:
    void accept()
    {
        while(!stop_)
        {
            pollfd waiting{listener_, POLLIN, 0};
            if(::poll(&waiting, 1, 20) <= 0)
                continue;
            const int downstream = ::accept(listener_, nullptr, nullptr);
            const int upstream = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(static_cast<uint16_t>(upstream_.load()));
            if(downstream < 0 || ::connect(upstream, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            {
                ::close(downstream);
                ::close(upstream);
                continue;
            }
            const int born = generation_;
            {
                std::lock_guard lock(sockets_mutex_);
                sockets_.push_back(downstream);
                sockets_.push_back(upstream);
            }
            pumps_.emplace_back([this, downstream, upstream, born] { pump(downstream, upstream, born, false); });
            pumps_.emplace_back([this, downstream, upstream, born] { pump(upstream, downstream, born, true); });
        }
    }
    void pump(int from, int to, int born, bool first_server_bytes)
    {
        char buffer[4096];
        while(!stop_)
        {
            pollfd waiting{from, POLLIN, 0};
            if(::poll(&waiting, 1, 20) <= 0)
                continue;
            const auto count = ::recv(from, buffer, sizeof(buffer), 0);
            if(count <= 0)
                return;
            if(first_server_bytes)
            {
                first_server_bytes = false;
                std::unique_lock lock(sockets_mutex_);
                if(slow_handshake_)
                {
                    if(!release_bytes_)
                        release_bytes_ = std::chrono::steady_clock::now() + 500ms;
                    if(std::chrono::steady_clock::now() < *release_bytes_)
                    {
                        ++delayed_handshakes_;
                        bytes_cv_.wait_until(lock, *release_bytes_, [&] { return stop_.load(); });
                    }
                }
            }
            if(stop_)
                return;
            if(born == generation_)
                ::send(to, buffer, static_cast<size_t>(count), MSG_NOSIGNAL);
        }
    }

    std::atomic<int> upstream_;
    std::atomic<int> generation_{0};
    int listener_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread acceptor_;
    std::vector<std::thread> pumps_;
    std::vector<int> sockets_;
    std::mutex sockets_mutex_;
    std::condition_variable bytes_cv_;
    bool slow_handshake_{};
    std::optional<std::chrono::steady_clock::time_point> release_bytes_;
    std::atomic<int> delayed_handshakes_{};
};

TEST(ClientChannelPolicy, FirstCallWaitsForASlowHandshakeWithinItsDeadline)
{
    Peer peer("slow");
    {
        SCOPED_TRACE("fresh Player channel Read with a 10 s deadline");
        Hop hop(peer.port);
        hop.delayHandshakes();
        sdk::ClientOptions options;
        options.catalog_endpoint = "127.0.0.1:" + std::to_string(peer.port);
        options.player_endpoint = "127.0.0.1:" + std::to_string(hop.port());
        auto client = sdk::Client::Connect(options);
        ASSERT_TRUE(client.ok()) << client.status();
        const auto end = std::chrono::system_clock::now() + 10s;
        auto read = client->read(1, {{0, 0}, {10, 0}}, end);
        ASSERT_TRUE(read.ok()) << read.status();
        auto item = read->next(end);
        EXPECT_TRUE(item.ok()) << item.status();
        if(item.ok())
        {
            ASSERT_TRUE(*item);
            ASSERT_TRUE((**item).completion);
            EXPECT_TRUE((**item).completion->complete);
        }
        EXPECT_GT(hop.delayedHandshakes(), 0);
    }
    {
        SCOPED_TRACE("fresh Client's first Acquire after catalog transport reconnects, with a 10 s deadline");
        Hop hop(peer.port);
        sdk::ClientOptions options;
        options.catalog_endpoint = "127.0.0.1:" + std::to_string(hop.port());
        auto client = sdk::Client::Connect(options);
        ASSERT_TRUE(client.ok()) << client.status();
        hop.delayHandshakes();
        auto writer = client->acquire(1, "writer", std::chrono::system_clock::now() + 10s);
        EXPECT_TRUE(writer.ok()) << writer.status();
        EXPECT_GT(hop.delayedHandshakes(), 0);
    }
    for(bool tail: {false, true})
    {
        SCOPED_TRACE(tail ? "fresh Player channel Tail" : "fresh Player channel physical Read");
        Hop hop(peer.port);
        hop.delayHandshakes();
        sdk::ClientOptions options;
        options.catalog_endpoint = "127.0.0.1:" + std::to_string(peer.port);
        options.player_endpoint = "127.0.0.1:" + std::to_string(hop.port());
        options.retry.max_retries = 0;
        auto client = sdk::Client::Connect(options);
        ASSERT_TRUE(client.ok()) << client.status();
        const auto end = std::chrono::system_clock::now() + 10s;
        if(tail)
        {
            auto stream = client->tail(1);
            ASSERT_TRUE(stream.ok()) << stream.status();
            auto item = stream->next(end);
            ASSERT_TRUE(item.ok()) << item.status();
            ASSERT_TRUE(*item);
            EXPECT_EQ((**item).events.size(), 1u);
        }
        else
        {
            auto stream = client->readPhysical(1, {0, 10}, end);
            ASSERT_TRUE(stream.ok()) << stream.status();
            auto item = stream->next(end);
            ASSERT_TRUE(item.ok()) << item.status();
            ASSERT_TRUE(*item);
            ASSERT_TRUE((**item).completion);
            EXPECT_TRUE((**item).completion->complete);
        }
        EXPECT_GT(hop.delayedHandshakes(), 0);
    }
    for(bool streaming: {false, true})
    {
        SCOPED_TRACE(streaming ? "fresh Keeper channel AppendStream without SDK retries"
                               : "fresh Keeper channel Append without SDK retries");
        Hop hop(peer.port);
        hop.delayHandshakes();
        Peer catalog("writer", "127.0.0.1:" + std::to_string(hop.port()));
        sdk::ClientOptions options;
        options.catalog_endpoint = "127.0.0.1:" + std::to_string(catalog.port);
        options.retry.max_retries = 0;
        auto client = sdk::Client::Connect(options);
        ASSERT_TRUE(client.ok()) << client.status();
        auto writer = client->acquire(1, "writer");
        ASSERT_TRUE(writer.ok()) << writer.status();
        const auto end = std::chrono::system_clock::now() + 10s;
        sdk::AppendSpec spec{{"", "slow handshake", "", "", {}}};
        if(streaming)
        {
            auto result = writer->appendBatch(std::span(&spec, 1), end);
            ASSERT_TRUE(result.ok()) << result.status();
            ASSERT_EQ(result->size(), 1u);
            ASSERT_TRUE(result->front().ok()) << result->front().status();
            EXPECT_TRUE(result->front()->acked());
        }
        else
        {
            auto result = writer->append(spec, end);
            ASSERT_TRUE(result.ok()) << result.status();
            EXPECT_TRUE(result->acked());
        }
        EXPECT_GT(hop.delayedHandshakes(), 0);
    }
}

TEST(ClientChannelPolicy, CallToADeadPeerEndsAtItsDeadline)
{
    Hop dead(0);
    const auto endpoint = "127.0.0.1:" + std::to_string(dead.port());
    Peer peer("catalog", endpoint);
    sdk::ClientOptions options;
    options.catalog_endpoint = "127.0.0.1:" + std::to_string(peer.port);
    options.player_endpoint = endpoint;
    options.retry.max_retries = 0;
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok()) << client.status();
    auto read = client->read(1, {{0, 0}, {10, 0}});
    ASSERT_TRUE(read.ok()) << read.status();
    EXPECT_EQ(read->next(std::chrono::system_clock::now() + 200ms).status().code(),
              absl::StatusCode::kDeadlineExceeded);
    auto physical = client->readPhysical(1, {0, 10});
    ASSERT_TRUE(physical.ok()) << physical.status();
    EXPECT_EQ(physical->next(std::chrono::system_clock::now() + 200ms).status().code(),
              absl::StatusCode::kDeadlineExceeded);
    // Tail has no RPC deadline: its pull watchdog must also cover synchronous stream creation while waiting for ready.
    auto tail = client->tail(1);
    ASSERT_TRUE(tail.ok()) << tail.status();
    EXPECT_EQ(tail->next(std::chrono::system_clock::now() + 200ms).status().code(),
              absl::StatusCode::kDeadlineExceeded);
    auto overall_tail = client->tail(1, {}, std::chrono::system_clock::now() + 200ms);
    ASSERT_TRUE(overall_tail.ok()) << overall_tail.status();
    EXPECT_EQ(overall_tail->next().status().code(), absl::StatusCode::kDeadlineExceeded);
    for(bool streaming: {false, true})
    {
        auto writer = client->acquire(1, "writer");
        ASSERT_TRUE(writer.ok()) << writer.status();
        sdk::AppendSpec spec{{"", "dead peer", "", "", {}}};
        const auto end = std::chrono::system_clock::now() + 200ms;
        const auto result =
                streaming ? writer->appendBatch(std::span(&spec, 1), end).status() : writer->append(spec, end).status();
        EXPECT_EQ(result.code(), absl::StatusCode::kDeadlineExceeded) << result;
    }
    auto writer = client->acquire(1, "writer");
    ASSERT_TRUE(writer.ok()) << writer.status();
    peer.stop();
    EXPECT_EQ(client->listChronicles(std::chrono::system_clock::now() + 200ms).status().code(),
              absl::StatusCode::kDeadlineExceeded);
    EXPECT_EQ(client->acquire(1, "writer", std::chrono::system_clock::now() + 200ms).status().code(),
              absl::StatusCode::kDeadlineExceeded);
    EXPECT_EQ(writer->release(std::chrono::system_clock::now() + 200ms).status().code(),
              absl::StatusCode::kDeadlineExceeded);
    options.player_endpoint.clear();
    Peer lookup("lookup");
    options.catalog_endpoint = "127.0.0.1:" + std::to_string(lookup.port);
    auto routed = sdk::Client::Connect(options);
    ASSERT_TRUE(routed.ok()) << routed.status();
    lookup.stop();
    EXPECT_EQ(routed->read(1, {{0, 0}, {10, 0}}, std::chrono::system_clock::now() + 200ms).status().code(),
              absl::StatusCode::kDeadlineExceeded);
}

// gRPC probes bandwidth with pings of its own, and one of them in flight when the path goes dark used to hold the
// keepalive ping back for the transport's one minute ping timeout, so both cases are covered. The bound is the
// longest the policy allows (keepalive time, keepalive timeout, DNS interval and maximum reconnect backoff, one
// second each), twice over for a loaded host; the caller has no other way out.
void CatalogCallReachesThePeerAfterTheHopGoesDark(bool bandwidth_probe)
{
    Peer before("before"), after("after");
    Hop hop(before.port);
    auto options = sdk::ClientOptions{};
    options.catalog_endpoint = "127.0.0.1:" + std::to_string(hop.port());
    options.channel_args[GRPC_ARG_HTTP2_BDP_PROBE] = static_cast<int>(bandwidth_probe);
    auto client = sdk::Client::Connect(options);
    ASSERT_TRUE(client.ok()) << client.status();
    auto answerOf = [&](std::chrono::system_clock::time_point deadline)
    {
        auto list = client->listChronicles(deadline);
        return list.ok() && !list->empty() ? list->front().name : std::string();
    };
    ASSERT_EQ(answerOf(std::chrono::system_clock::now() + 10s), "before");
    hop.goDark(after.port);
    const auto end = std::chrono::system_clock::now() + 8s;
    std::string answer;
    // The call in flight on the dead connection fails UNAVAILABLE once the ping goes unanswered. The caller sends the
    // next one over the connection the channel makes again.
    while(answer.empty() && std::chrono::system_clock::now() < end)
    {
        answer = answerOf(end);
        if(answer.empty())
            std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(answer, "after");
}

TEST(ClientChannelPolicy, CatalogTargetAcceptsEndpointsAndResolvesReplicaLists)
{
    for(const auto& endpoint: {"localhost:50051",
                               "127.0.0.1:50051",
                               "[::1]:50051",
                               "dns:///localhost:50051",
                               "unix:///tmp/chronolog.sock",
                               "ipv4:127.0.0.1:1,127.0.0.1:2",
                               "ipv4:///127.0.0.1:1,127.0.0.1:2",
                               "ipv6:[::1]:1,[::1]:2"})
    {
        auto target = sdk::detail::catalogTarget(endpoint);
        ASSERT_TRUE(target.ok()) << endpoint << ": " << target.status();
        EXPECT_EQ(*target, endpoint);
    }
    for(const auto& endpoints: {"127.0.0.1:1,127.0.0.1:2", "localhost:1,127.0.0.1:2", "localhost:1,localhost:2"})
    {
        auto target = sdk::detail::catalogTarget(endpoints);
        ASSERT_TRUE(target.ok()) << endpoints << ": " << target.status();
        EXPECT_EQ(*target, "ipv4:127.0.0.1:1,127.0.0.1:2");
    }
}
TEST(ClientChannelPolicy, CatalogTargetRejectsMalformedEndpointsBeforeConnecting)
{
    for(const auto& endpoints: {"",
                                "localhost",
                                ":50051",
                                "localhost:",
                                "localhost:abc",
                                "localhost:0",
                                "localhost:65536",
                                "localhost:1,",
                                ",localhost:1",
                                "localhost:1,,localhost:2",
                                "localhost:1,missing-port",
                                "localhost:1,localhost:-1",
                                "localhost:1,localhost:2x",
                                "localhost:1, localhost:2",
                                "localhost:1,[::1]:2",
                                "[invalid]:1",
                                "ipv4:",
                                "ipv4:localhost:1,127.0.0.1:2",
                                "ipv4:999.0.0.1:1",
                                "dns:///localhost:1,localhost:2"})
    {
        auto target = sdk::detail::catalogTarget(endpoints);
        EXPECT_EQ(target.status().code(), absl::StatusCode::kInvalidArgument) << endpoints << ": " << target.status();
        auto options = sdk::ClientOptions{};
        options.catalog_endpoint = endpoints;
        auto client = sdk::Client::Connect(options);
        EXPECT_EQ(client.status().code(), absl::StatusCode::kInvalidArgument) << endpoints << ": " << client.status();
    }
}

TEST(ClientChannelPolicy, ClientReachesTheCatalogThroughTheNextReplicaWhenItsVisorDies)
{
    Peer first("first"), second("second"), third("third");
    auto options = sdk::ClientOptions{};
    options.catalog_endpoint = "localhost:" + std::to_string(first.port) + ",127.0.0.1:" + std::to_string(second.port) +
                               ",localhost:" + std::to_string(third.port);
    auto client = sdk::Client::Connect(options, std::chrono::system_clock::now() + 2s);
    ASSERT_TRUE(client.ok()) << client.status();
    auto initial = client->listChronicles();
    ASSERT_TRUE(initial.ok()) << initial.status();
    ASSERT_EQ(initial->size(), 1u);
    ASSERT_EQ(initial->front().name, "first");
    first.stop();
    const auto end = std::chrono::system_clock::now() + 8s;
    std::string answer;
    absl::Status last;
    for(int attempt = 0; attempt < 400 && std::chrono::system_clock::now() < end; ++attempt)
    {
        auto result = client->listChronicles(end);
        last = result.status();
        if(result.ok() && !result->empty())
        {
            answer = result->front().name;
            break;
        }
        if(!absl::IsUnavailable(last))
            break;
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_EQ(answer, "second") << last;
}

TEST(ClientChannelPolicy, ACatalogCallThroughAHopThatStopsForwardingReachesTheSuccessorWithinTheKeepaliveBound)
{
    CatalogCallReachesThePeerAfterTheHopGoesDark(true);
}

TEST(ClientChannelPolicy, TheKeepalivePingAloneNoticesAHopThatStopsForwarding)
{
    CatalogCallReachesThePeerAfterTheHopGoesDark(false);
}
} // namespace
