#include <gtest/gtest.h>

#include "rpc/FlakyResolver.h"
#include "rpc/Channel.h"
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
    ListMembers(grpc::ServerContext*, const iv1::ListMembersRequest*, iv1::MembershipResponse* response) override
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

std::string Ask(const std::shared_ptr<grpc::Channel>& channel, std::string* error = nullptr)
{
    grpc::ClientContext context;
    withTimeout(context, std::chrono::seconds(10));
    iv1::MembershipResponse response;
    auto status = iv1::Cluster::NewStub(channel)->ListMembers(&context, iv1::ListMembersRequest(), &response);
    if(error)
        *error = status.error_message();
    return status.ok() ? response.status().message() : "";
}
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
