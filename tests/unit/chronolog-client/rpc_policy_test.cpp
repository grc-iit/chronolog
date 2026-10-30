#include <gtest/gtest.h>
#include <ClientConfiguration.h>
#include <RpcPolicy.h>
#include "RpcPolicyContext.h"
#include <chronolog_client.h>
#include <limits>
#include <type_traits>
#include <fstream>
#include <cstdio>
#include <unistd.h>

TEST(ClientRpcPolicy, RejectsInvalidValues)
{
    for(auto value: {"0", "-1", "18446744073709551615", "1.5", "true", "\"5000\"", "null", "[]", "{}"})
    {
        char path[] = "/tmp/chronolog-policy-XXXXXX";
        int fd = mkstemp(path);
        ASSERT_NE(fd, -1);
        close(fd);
        {
            std::ofstream f(path);
            f << "{\"chrono_client\":{\"RpcPolicy\":{\"append_ms\":" << value << "}}}";
        }
        chronolog::ClientConfiguration conf;
        EXPECT_FALSE(conf.load_from_file(path)) << value;
        std::remove(path);
    }
}

namespace
{
bool loadPolicy(chronolog::ClientConfiguration& conf, char const* json)
{
    char path[] = "/tmp/chronolog-policy-XXXXXX";
    int fd = mkstemp(path);
    EXPECT_NE(fd, -1);
    if(fd == -1)
        return false;
    close(fd);
    {
        std::ofstream f(path);
        f << json;
    }
    bool loaded = conf.load_from_file(path);
    std::remove(path);
    return loaded;
}
} // namespace

TEST(ClientRpcPolicy, LoadsDefaultsAndOverridesThroughPortalConfiguration)
{
    chronolog::ClientConfiguration first, second;
    ASSERT_TRUE(loadPolicy(first, R"({"chrono_client":{}})"));
    auto const& policy = first.PORTAL_CONF.RPC_POLICY;
    EXPECT_EQ(policy.append_ms, 5000u);
    EXPECT_EQ(policy.control_ms, 5000u);
    EXPECT_EQ(policy.tail_ms, 5000u);
    EXPECT_EQ(policy.replay_ms, 180000u);
    EXPECT_EQ(policy.attempt_ms, 5000u);
    EXPECT_EQ(policy.startup_ms, 30000u);
    EXPECT_EQ(policy.shutdown_ms, 30000u);
    EXPECT_EQ(policy.transfer_ms, 30000u);
    ASSERT_TRUE(loadPolicy(
            first,
            R"({"chrono_client":{"RpcPolicy":{"append_ms":17,"control_ms":18,"tail_ms":19,"replay_ms":20,"attempt_ms":21,"startup_ms":22,"shutdown_ms":23,"transfer_ms":24}}})"));
    EXPECT_EQ(policy.append_ms, 17u);
    EXPECT_EQ(policy.control_ms, 18u);
    EXPECT_EQ(policy.tail_ms, 19u);
    EXPECT_EQ(policy.replay_ms, 20u);
    EXPECT_EQ(policy.attempt_ms, 21u);
    EXPECT_EQ(policy.startup_ms, 22u);
    EXPECT_EQ(policy.shutdown_ms, 23u);
    EXPECT_EQ(policy.transfer_ms, 24u);
    EXPECT_EQ(second.PORTAL_CONF.RPC_POLICY.append_ms, 5000u);
}

TEST(ClientRpcPolicy, RejectsNonObjectPolicy)
{
    chronolog::ClientConfiguration conf;
    for(auto text: {R"({"chrono_client":{"RpcPolicy":null}})",
                    R"({"chrono_client":{"RpcPolicy":[]}})",
                    R"({"chrono_client":{"RpcPolicy":5000}})"})
        EXPECT_FALSE(loadPolicy(conf, text)) << text;
}

TEST(RpcPolicy, ValidatesEveryBudgetWithoutRequiringAttemptLessThanOperation)
{
    using P = chronolog::RpcTimeoutPolicy;
    uint64_t P::*members[] = {&P::append_ms,
                              &P::control_ms,
                              &P::tail_ms,
                              &P::replay_ms,
                              &P::attempt_ms,
                              &P::startup_ms,
                              &P::shutdown_ms,
                              &P::transfer_ms};
    EXPECT_EQ(chronolog::validateRpcTimeoutPolicy(P{}), chronolog::CL_SUCCESS);
    for(auto member: members)
    {
        for(uint64_t bad: {uint64_t{0}, std::numeric_limits<uint64_t>::max(), uint64_t{9223372036855}})
        {
            P policy;
            policy.*member = bad;
            EXPECT_EQ(chronolog::validateRpcTimeoutPolicy(policy), chronolog::CL_ERR_INVALID_ARG);
        }
    }
    P policy;
    policy.append_ms = 1;
    policy.attempt_ms = 30000;
    EXPECT_EQ(chronolog::validateRpcTimeoutPolicy(policy), chronolog::CL_SUCCESS);
}

TEST(RpcPolicy, PreservesExistingCodesAndNamesNewLocalResults)
{
    EXPECT_EQ(chronolog::CL_ERR_PROTOCOL_VERSION_MISMATCH, -13);
    EXPECT_EQ(chronolog::CL_ERR_PARTIAL_RESULT, -18);
    struct NamedStatus
    {
        chronolog::ClientErrorCode status;
        int code;
        char const* name;
    };
    NamedStatus values[] = {{chronolog::CL_ERR_DEADLINE_EXCEEDED, -22, "CL_ERR_DEADLINE_EXCEEDED"},
                            {chronolog::CL_ERR_OUTCOME_UNCONFIRMED, -23, "CL_ERR_OUTCOME_UNCONFIRMED"},
                            {chronolog::CL_ERR_RPC_CAPACITY, -24, "CL_ERR_RPC_CAPACITY"},
                            {chronolog::CL_ERR_CLOCK_UNAVAILABLE, -25, "CL_ERR_CLOCK_UNAVAILABLE"},
                            {chronolog::CL_ERR_TIMESTAMP_EXHAUSTED, -26, "CL_ERR_TIMESTAMP_EXHAUSTED"},
                            {chronolog::CL_ERR_SHUTDOWN_INCOMPLETE, -27, "CL_ERR_SHUTDOWN_INCOMPLETE"},
                            {chronolog::CL_ERR_NATIVE_RUNTIME_UNSUPPORTED, -28, "CL_ERR_NATIVE_RUNTIME_UNSUPPORTED"}};
    for(auto const& value: values)
    {
        EXPECT_EQ(value.status, value.code);
        EXPECT_STREQ(chronolog::to_string(value.status), value.name);
        EXPECT_STREQ(chronolog::to_string_client(value.code), value.name);
    }
}

TEST(RpcPolicy, PreservesExistingClientConstructorAndSetterSignatures)
{
    static_assert(std::is_constructible_v<chronolog::Client, chronolog::ClientPortalServiceConf const&>);
    static_assert(std::is_constructible_v<chronolog::Client,
                                          chronolog::ClientPortalServiceConf const&,
                                          chronolog::ClientQueryServiceConf const&>);
    static_assert(std::is_same_v<decltype(&chronolog::Client::SetRpcTimeoutPolicy),
                                 int (chronolog::Client::*)(chronolog::RpcTimeoutPolicy const&)>);
    chronolog::ClientPortalServiceConf legacy{"ofi+sockets", "127.0.0.1", 5555, 55};
    EXPECT_EQ(legacy.RPC_POLICY.append_ms, 5000u);
    EXPECT_EQ(chronolog::CLIENT_PROTOCOL_VERSION, 3u);
}


TEST(RpcPolicyContext, LiveSnapshotRefusesMutationAndKeepsItsOriginalPolicy)
{
    chronolog::RpcPolicyContext context(chronolog::RpcTimeoutPolicy{});
    chronolog::RpcTimeoutPolicy changed;
    changed.append_ms = 17;
    {
        auto snapshot = context.tryAcquire();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->policy().append_ms, 5000u);
        EXPECT_EQ(context.setPolicy(changed), chronolog::CL_ERR_RPC_CAPACITY);
        EXPECT_EQ(snapshot->policy().append_ms, 5000u);
    }
    EXPECT_EQ(context.setPolicy(changed), chronolog::CL_SUCCESS);
    auto snapshot = context.tryAcquire();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->policy().append_ms, 17u);
}

TEST(RpcPolicyContext, IndependentContextsDoNotShareSettings)
{
    chronolog::RpcPolicyContext first(chronolog::RpcTimeoutPolicy{}), second(chronolog::RpcTimeoutPolicy{});
    auto& shared = first;
    chronolog::RpcTimeoutPolicy changed;
    changed.append_ms = 17;
    EXPECT_EQ(shared.setPolicy(changed), chronolog::CL_SUCCESS);
    auto firstSnapshot = first.tryAcquire();
    auto secondSnapshot = second.tryAcquire();
    ASSERT_TRUE(firstSnapshot);
    ASSERT_TRUE(secondSnapshot);
    EXPECT_EQ(firstSnapshot->policy().append_ms, 17u);
    EXPECT_EQ(secondSnapshot->policy().append_ms, 5000u);
}

TEST(RpcPolicyContext, FirstConnectionAttemptPermanentlyFreezesPolicy)
{
    chronolog::RpcPolicyContext context(chronolog::RpcTimeoutPolicy{});
    context.freeze();
    chronolog::RpcTimeoutPolicy changed;
    changed.append_ms = 17;
    EXPECT_EQ(context.setPolicy(changed), chronolog::CL_ERR_RPC_CAPACITY);
    changed.append_ms = 0;
    EXPECT_EQ(context.setPolicy(changed), chronolog::CL_ERR_INVALID_ARG);
    auto snapshot = context.tryAcquire();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->policy().append_ms, 5000u);
}

TEST(RpcPolicyContext, MovedSnapshotsReleaseAdmissionExactlyOnceAndCanOutliveContext)
{
    using Snapshot = chronolog::RpcPolicyContext::Snapshot;
    std::optional<Snapshot> survivor;
    {
        chronolog::RpcPolicyContext context(chronolog::RpcTimeoutPolicy{});
        auto snapshot = context.tryAcquire();
        ASSERT_TRUE(snapshot);
        survivor.emplace(std::move(*snapshot));
    }
    EXPECT_EQ(survivor->policy().append_ms, 5000u);
    survivor.reset();
    chronolog::RpcPolicyContext context(chronolog::RpcTimeoutPolicy{});
    {
        auto snapshot = context.tryAcquire();
        auto moved = std::move(*snapshot);
    }
    EXPECT_EQ(context.setPolicy(chronolog::RpcTimeoutPolicy{}), chronolog::CL_SUCCESS);
}
