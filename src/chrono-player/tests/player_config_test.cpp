#include <gtest/gtest.h>
#include <map>
#include "chrono-player/PlayerConfig.h"

namespace chronolog::player
{
namespace
{

PlayerConfig::Getenv env(std::map<std::string, std::string> values)
{
    return [values = std::move(values)](const char* name) -> const char*
    {
        auto it = values.find(name);
        return it == values.end() ? nullptr : it->second.c_str();
    };
}

TEST(player_config, DefaultsMatchTheSpec)
{
    auto cfg = PlayerConfig::load(std::nullopt, env({}));
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->listen, "0.0.0.0:50054");
    EXPECT_EQ(cfg->batch_size, 1024u);
    EXPECT_EQ(cfg->read_max_events, 262144u);
    EXPECT_EQ(cfg->tail_poll_ms, 200u);
    EXPECT_EQ(cfg->keeper_deadline_ms, 2000u);
    EXPECT_TRUE(cfg->archive_root.empty());
    EXPECT_EQ(cfg->manifest_poll_ms, 1000u);
}

TEST(player_config, EnvironmentOverrides)
{
    auto cfg = PlayerConfig::load(std::nullopt,
                                  env({{"CHRONOLOG_PLAYER_LISTEN", "127.0.0.1:1"},
                                       {"CHRONOLOG_PLAYER_BATCH_SIZE", "7"},
                                       {"CHRONOLOG_PLAYER_READ_MAX_EVENTS", "100"},
                                       {"CHRONOLOG_PLAYER_ARCHIVE_ROOT", "/archive"},
                                       {"CHRONOLOG_PLAYER_MANIFEST_POLL_MS", "1500"},
                                       {"CHRONOLOG_PLAYER_KEEPER_INTERNAL", "k1=h:1,k2=h:2"}}));
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->listen, "127.0.0.1:1");
    EXPECT_EQ(cfg->batch_size, 7u);
    EXPECT_EQ(cfg->read_max_events, 100u);
    EXPECT_EQ(cfg->archive_root, "/archive");
    EXPECT_EQ(cfg->manifest_poll_ms, 1500u);
    EXPECT_EQ(cfg->keeperInternal({"k2", "x:9"}), "h:2");
}

TEST(player_config, KeeperInternalAddressFallsBackToTheEndpointHostPlusSuffix)
{
    auto cfg = PlayerConfig::load(std::nullopt, env({}));
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->keeperInternal({"k", "chrono-keeper:50052"}), "chrono-keeper:50062");
}

TEST(player_config, RejectsBadValues)
{
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_READ_MAX_EVENTS", "0"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_MANIFEST_POLL_MS", "0"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_BATCH_SIZE", "0"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_TAIL_POLL_MS", "x"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_KEEPER_INTERNAL", "nopair"}})).ok());
}

} // namespace
} // namespace chronolog::player
