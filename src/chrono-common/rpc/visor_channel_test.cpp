#include <gtest/gtest.h>

#include "rpc/FlakyResolver.h"
#include "rpc/VisorChannel.h"
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

std::unique_ptr<grpc::Server> Start(Replica& replica, int* port)
{
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), port);
    builder.RegisterService(&replica);
    return builder.BuildAndStart();
}

std::string Ask(const std::shared_ptr<grpc::Channel>& channel, std::string* error = nullptr)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    context.set_wait_for_ready(true);
    iv1::MembershipResponse response;
    auto status = iv1::Cluster::NewStub(channel)->ListMembers(&context, iv1::ListMembersRequest(), &response);
    if(error)
        *error = status.error_message();
    return status.ok() ? response.status().message() : "";
}
} // namespace

TEST(VisorChannelTest, OneEndpointIsUnchangedAndAListBecomesOnePickFirstTarget)
{
    EXPECT_EQ(visorTarget("chrono-visor:50061"), "chrono-visor:50061");
    EXPECT_EQ(visorTarget("127.0.0.1:1,127.0.0.1:2"), "ipv4:127.0.0.1:1,127.0.0.1:2");
}

// Section 12 leader kill: a client configured with one dead replica first keeps working through the others.
TEST(VisorChannelTest, CallsFailOverWhenTheAttachedReplicaDies)
{
    Replica first("first"), second("second");
    int first_port = 0, second_port = 0;
    auto a = Start(first, &first_port);
    auto b = Start(second, &second_port);
    ASSERT_TRUE(a && b);
    auto channel =
            visorChannel("127.0.0.1:" + std::to_string(first_port) + ",127.0.0.1:" + std::to_string(second_port));
    EXPECT_EQ(Ask(channel), "first");
    a->Shutdown(std::chrono::system_clock::now());
    a.reset();
    std::string answer;
    for(int attempt = 0; attempt < 50 && answer.empty(); ++attempt) answer = Ask(channel);
    EXPECT_EQ(answer, "second");
}

// FLAKE-2: a lookup that fails once must be retried within the call deadline, not after 30 s.
TEST(VisorChannelTest, ACallSurvivesATransientNameResolutionFailure)
{
    Replica only("only");
    int port = 0;
    auto server = Start(only, &port);
    ASSERT_TRUE(server);
    test::lookups = 0;
    test::failing_lookups = 1;
    auto channel = visorChannel("visor.test:" + std::to_string(port));
    EXPECT_EQ(Ask(channel), "only");
    EXPECT_GE(test::lookups.load(), 2);
}
} // namespace chronolog::rpc
