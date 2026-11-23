#include <gtest/gtest.h>
#include <ChronoPlayerConfiguration.h>
#include <string>

TEST(PlayerRpcPolicy, RejectsInvalidValues)
{
    for(auto value: {"0", "-1", "18446744073709551615", "1.5", "true", "\"5000\"", "null", "[]", "{}"})
    {
        chronolog::PlayerConfiguration conf;
        auto text = std::string("{\"RpcPolicy\":{\"append_ms\":") + value + "}}";
        auto* root = json_tokener_parse(text.c_str());
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(conf.parseJsonConf(root), chronolog::CL_ERR_INVALID_CONF) << text;
        json_object_put(root);
    }
}

TEST(PlayerRpcPolicy, LoadsDefaultsAndIndependentOverrides)
{
    chronolog::PlayerConfiguration first, second;
    auto* root = json_tokener_parse("{}");
    ASSERT_EQ(first.parseJsonConf(root), chronolog::CL_SUCCESS);
    json_object_put(root);
    EXPECT_EQ(first.RPC_POLICY.append_ms, 5000u);
    EXPECT_EQ(first.RPC_POLICY.control_ms, 5000u);
    EXPECT_EQ(first.RPC_POLICY.tail_ms, 5000u);
    EXPECT_EQ(first.RPC_POLICY.replay_ms, 180000u);
    EXPECT_EQ(first.RPC_POLICY.attempt_ms, 5000u);
    EXPECT_EQ(first.RPC_POLICY.startup_ms, 30000u);
    EXPECT_EQ(first.RPC_POLICY.shutdown_ms, 30000u);
    EXPECT_EQ(first.RPC_POLICY.transfer_ms, 30000u);
    root = json_tokener_parse(
            R"({"RpcPolicy":{"append_ms":17,"control_ms":18,"tail_ms":19,"replay_ms":20,"attempt_ms":21,"startup_ms":22,"shutdown_ms":23,"transfer_ms":24}})");
    ASSERT_EQ(first.parseJsonConf(root), chronolog::CL_SUCCESS);
    json_object_put(root);
    EXPECT_EQ(first.RPC_POLICY.append_ms, 17u);
    EXPECT_EQ(first.RPC_POLICY.control_ms, 18u);
    EXPECT_EQ(first.RPC_POLICY.tail_ms, 19u);
    EXPECT_EQ(first.RPC_POLICY.replay_ms, 20u);
    EXPECT_EQ(first.RPC_POLICY.attempt_ms, 21u);
    EXPECT_EQ(first.RPC_POLICY.startup_ms, 22u);
    EXPECT_EQ(first.RPC_POLICY.shutdown_ms, 23u);
    EXPECT_EQ(first.RPC_POLICY.transfer_ms, 24u);
    EXPECT_EQ(second.RPC_POLICY.append_ms, 5000u);
}

TEST(PlayerRpcPolicy, RejectsNonObjectPolicy)
{
    for(auto text: {R"({"RpcPolicy":null})", R"({"RpcPolicy":[]})", R"({"RpcPolicy":5000})"})
    {
        chronolog::PlayerConfiguration conf;
        auto* root = json_tokener_parse(text);
        EXPECT_EQ(conf.parseJsonConf(root), chronolog::CL_ERR_INVALID_CONF);
        json_object_put(root);
    }
}
