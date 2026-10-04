#include <gtest/gtest.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "common/rpc/FlakyResolver.h"
#include "common/rpc/Channel.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

namespace chronolog::rpc
{
namespace
{
namespace iv1 = chronolog::internal::v1;

class Replica final: public iv1::Cluster::Service
{
public:
    explicit Replica(std::string name)
        : name_(std::move(name))
    {}
    grpc::Status
    ListMembers(grpc::ServerContext*, const iv1::ListMembersRequest*, iv1::ListMembersResponse* response) override
    {
        response->mutable_status()->set_message(name_);
        return grpc::Status::OK;
    }
    grpc::Status
    Heartbeat(grpc::ServerContext*, const iv1::HeartbeatRequest*, iv1::HeartbeatResponse* response) override
    {
        response->mutable_status()->set_message(name_);
        return grpc::Status::OK;
    }

private:
    std::string name_;
};

std::unique_ptr<grpc::Server> Start(Replica& replica, int* port, const std::string& host = "127.0.0.1")
{
    grpc::ServerBuilder builder;
    applyServerPolicy(builder);
    builder.AddListeningPort(host + ":" + std::to_string(*port), grpc::InsecureServerCredentials(), port);
    builder.RegisterService(&replica);
    return builder.BuildAndStart();
}

std::string Ask(const std::shared_ptr<grpc::Channel>& channel,
                std::string* error = nullptr,
                std::chrono::milliseconds timeout = std::chrono::seconds(10))
{
    grpc::ClientContext context;
    withTimeout(context, timeout);
    iv1::ListMembersResponse response;
    auto status = iv1::Cluster::NewStub(channel)->ListMembers(&context, iv1::ListMembersRequest(), &response);
    if(error)
        *error = status.error_message();
    return status.ok() ? response.status().message() : "";
}

// A TCP hop in front of one peer. Dark, it keeps every connection open and forwards nothing, the way a dropped
// route or a dead switch looks to both ends.
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
    void goDark() { dark_ = true; }

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
            address.sin_port = htons(static_cast<uint16_t>(upstream_));
            if(downstream < 0 || ::connect(upstream, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
            {
                ::close(downstream);
                ::close(upstream);
                continue;
            }
            sockets_.push_back(downstream);
            sockets_.push_back(upstream);
            pumps_.emplace_back([this, downstream, upstream] { pump(downstream, upstream); });
            pumps_.emplace_back([this, downstream, upstream] { pump(upstream, downstream); });
        }
    }
    void pump(int from, int to)
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
            if(!dark_)
                ::send(to, buffer, static_cast<size_t>(count), MSG_NOSIGNAL);
        }
    }

    const int upstream_;
    int listener_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> dark_{false};
    std::thread acceptor_;
    std::vector<std::thread> pumps_;
    std::vector<int> sockets_;
};
} // namespace

TEST(ChannelPoolTest, OneEndpointIsUnchangedAndAListBecomesOnePickFirstTarget)
{
    EXPECT_EQ(visorTarget("chrono-visor:50061"), "chrono-visor:50061");
    EXPECT_EQ(visorTarget("127.0.0.1:1,127.0.0.1:2"), "ipv4:127.0.0.1:1,127.0.0.1:2");
}

// Section 12 leader kill: a client configured with one dead replica first keeps working through the others.
TEST(ChannelPoolTest, CallsFailOverWhenTheAttachedReplicaDies)
{
    Replica first("first"), second("second");
    int first_port = 0, second_port = 0;
    auto a = Start(first, &first_port);
    auto b = Start(second, &second_port);
    ASSERT_TRUE(a && b);
    ChannelPool pool;
    auto channel = pool.get("127.0.0.1:" + std::to_string(first_port) + ",127.0.0.1:" + std::to_string(second_port));
    EXPECT_EQ(Ask(channel), "first");
    a->Shutdown(std::chrono::system_clock::now());
    a.reset();
    std::string answer;
    for(int attempt = 0; attempt < 50 && answer.empty(); ++attempt) answer = Ask(channel);
    EXPECT_EQ(answer, "second");
}

// FLAKE-2: a lookup that fails once must be retried within the call deadline, not after 30 s.
TEST(ChannelPoolTest, ACallSurvivesATransientNameResolutionFailure)
{
    Replica only("only");
    int port = 0;
    auto server = Start(only, &port);
    ASSERT_TRUE(server);
    test::lookups = 0;
    test::failing_lookups = 1;
    ChannelPool pool;
    auto channel = pool.get("visor.test:" + std::to_string(port));
    EXPECT_EQ(Ask(channel), "only");
    EXPECT_GE(test::lookups.load(), 2);
}

// A peer that stops and returns on another address behind the same name is reached again by the same
// channel, with no new caller and no fixed address.
TEST(ChannelPoolTest, APeerThatReturnsOnANewAddressIsReachedByTheSameChannel)
{
    Replica before("before"), after("after");
    int port = 0;
    auto server = Start(before, &port);
    ASSERT_TRUE(server);
    test::failing_lookups = 0;
    test::address = "127.0.0.1";
    ChannelPool pool;
    auto channel = pool.get("visor.test:" + std::to_string(port));
    ASSERT_EQ(Ask(channel), "before");
    server->Shutdown(std::chrono::system_clock::now());
    server.reset();
    test::address = "127.0.0.2";
    int same_port = port;
    auto moved = Start(after, &same_port, "127.0.0.2");
    ASSERT_TRUE(moved);
    ASSERT_EQ(same_port, port);
    EXPECT_EQ(Ask(channel), "after");
    EXPECT_EQ(pool.created(), 1u);
    test::address = "127.0.0.1";
}

// A hop that stops forwarding without closing gives the channel nothing but silence. Only a ping that goes unanswered
// notices it, after which the name is looked up again and the next call reaches the peer that took over behind it.
// The deadline is the longest the policy allows, twice over for a loaded host; the caller has no other way out.
// gRPC probes bandwidth with pings of its own, and one of them in flight when the path goes dark used to hold the
// keepalive ping back for the transport's one minute ping timeout, so both cases are covered.
void HeartbeatReachesTheNewLeaderAfterTheHopGoesDark(bool bandwidth_probe)
{
    Replica before("before"), after("after");
    int upstream_port = 0;
    auto server = Start(before, &upstream_port);
    ASSERT_TRUE(server);
    Hop hop(upstream_port);
    int port = hop.port();
    auto successor = Start(after, &port, "127.0.0.2");
    ASSERT_TRUE(successor);
    ASSERT_EQ(port, hop.port());
    test::failing_lookups = 0;
    test::address = "127.0.0.1";
    auto args = channelArguments();
    args.SetInt(GRPC_ARG_HTTP2_BDP_PROBE, bandwidth_probe);
    auto channel =
            grpc::CreateCustomChannel("visor.test:" + std::to_string(port), grpc::InsecureChannelCredentials(), args);
    auto heartbeat = [&](std::chrono::milliseconds timeout)
    {
        grpc::ClientContext context;
        withTimeout(context, timeout);
        iv1::HeartbeatResponse response;
        auto status = iv1::Cluster::NewStub(channel)->Heartbeat(&context, iv1::HeartbeatRequest(), &response);
        return status.ok() ? response.status().message() : std::string();
    };
    ASSERT_EQ(heartbeat(std::chrono::seconds(10)), "before");
    hop.goDark();
    test::address = "127.0.0.2";
    const std::chrono::milliseconds bound{
            2 * (kKeepaliveTimeMs + kKeepaliveTimeoutMs + kDnsMinResolveIntervalMs + kMaxBackoffMs)};
    const auto end = std::chrono::steady_clock::now() + bound;
    std::string answer;
    // The call in flight on the dead connection fails UNAVAILABLE once the ping goes unanswered. The caller, like the
    // Keeper's heartbeat loop, sends the next one, which waits for the channel to be ready again.
    while(answer.empty() && std::chrono::steady_clock::now() < end)
        answer = heartbeat(
                std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()));
    EXPECT_EQ(answer, "after");
    test::address = "127.0.0.1";
}

TEST(ChannelPoolTest, AHeartbeatThroughAHopThatStopsForwardingReachesTheNewLeaderWithinTheKeepaliveBound)
{
    HeartbeatReachesTheNewLeaderAfterTheHopGoesDark(true);
}

TEST(ChannelPoolTest, TheKeepalivePingAloneNoticesAHopThatStopsForwarding)
{
    HeartbeatReachesTheNewLeaderAfterTheHopGoesDark(false);
}

// Every caller of one target shares one channel; the Visor forwarding path relies on it.
TEST(ChannelPoolTest, ManyCallsToOnePeerCreateOneChannel)
{
    Replica only("only");
    int port = 0;
    auto server = Start(only, &port);
    ASSERT_TRUE(server);
    ChannelPool pool;
    const auto target = "127.0.0.1:" + std::to_string(port);
    for(int call = 0; call < 20; ++call) EXPECT_EQ(Ask(pool.get(target)), "only");
    EXPECT_EQ(pool.created(), 1u);
    EXPECT_EQ(pool.get(target), pool.get(target));
    int other_port = 0;
    Replica second("second");
    auto other = Start(second, &other_port);
    ASSERT_TRUE(other);
    EXPECT_EQ(Ask(pool.get("127.0.0.1:" + std::to_string(other_port))), "second");
    EXPECT_EQ(pool.created(), 2u);
}

TEST(ChannelPoolTest, LivenessDeadlineIsShorterThanEveryTimerItFeeds)
{
    using std::chrono::milliseconds;
    const auto deadline = livenessDeadline({milliseconds(10000), milliseconds(2000), milliseconds(5000)});
    EXPECT_LT(deadline, milliseconds(2000));
    EXPECT_GE(deadline, kMinLivenessDeadline);
}
} // namespace chronolog::rpc
