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
#include <string>
#include <thread>
#include <vector>
#include "chronolog/client/client.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{
namespace wire = chronolog::v1;
namespace sdk = chronolog::client;
using namespace std::chrono_literals;

class Peer final: public wire::Catalog::Service
{
public:
    explicit Peer(std::string name)
        : name_(std::move(name))
    {
        grpc::ServerBuilder builder;
        // The server policy of every ChronoLog service, so the client's keepalive pings are not strikes.
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 500);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(this);
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
    int port = 0;

private:
    std::string name_;
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
            sockets_.push_back(downstream);
            sockets_.push_back(upstream);
            pumps_.emplace_back([this, downstream, upstream, born] { pump(downstream, upstream, born); });
            pumps_.emplace_back([this, downstream, upstream, born] { pump(upstream, downstream, born); });
        }
    }
    void pump(int from, int to, int born)
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
};

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
