#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
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

TEST(player_config, AcceptsTierConfigKeysAndRejectsInvalidTables)
{
    nlohmann::json tiers = nlohmann::json::array({{{"name", "local"},
                                                   {"kind", "posix"},
                                                   {"root", "/archive"},
                                                   {"rank", 0},
                                                   {"tier_uuid", "local-uuid"},
                                                   {"f_type", 1},
                                                   {"st_dev", 2},
                                                   {"f_fsid", {3, 4}}},
                                                  {{"name", "slow"},
                                                   {"kind", "posix"},
                                                   {"root", "/slow"},
                                                   {"rank", 1},
                                                   {"tier_uuid", "slow-uuid"},
                                                   {"f_type", 1},
                                                   {"st_dev", 2},
                                                   {"f_fsid", {3, 4}},
                                                   {"budget_bytes", 100},
                                                   {"min_free_fraction", 0.1},
                                                   {"high_watermark", 0.9},
                                                   {"low_watermark", 0.7}}});
    auto load = [&](const nlohmann::json& table, std::string deployment = "test")
    {
        return PlayerConfig::load(std::nullopt,
                                  env({{"CHRONOLOG_PLAYER_TIERS", table.dump()},
                                       {"CHRONOLOG_PLAYER_DEPLOYMENT_ID", deployment},
                                       {"CHRONOLOG_PLAYER_ARCHIVE_ROOT", "/archive"},
                                       {"CHRONOLOG_PLAYER_TIER_IO_TIMEOUT_MS", "42"},
                                       {"CHRONOLOG_PLAYER_TIER_PROBE_TIMEOUT_MS", "43"},
                                       {"CHRONOLOG_PLAYER_TIER_PROBE_INTERVAL_MS", "44"},
                                       {"CHRONOLOG_PLAYER_SLOW_TIER_IO_THREADS", "3"}}));
    };
    auto cfg = load(tiers);
    ASSERT_TRUE(cfg.ok()) << cfg.status();
    ASSERT_EQ(cfg->tiers.size(), 2u);
    EXPECT_EQ(cfg->tiers[1].root, "/slow");
    EXPECT_EQ(cfg->tier_io_timeout_ms, 42u);
    EXPECT_EQ(cfg->tier_probe_timeout_ms, 43u);
    EXPECT_EQ(cfg->tier_probe_interval_ms, 44u);
    EXPECT_EQ(cfg->slow_tier_io_threads, 3u);
    EXPECT_FALSE(load(tiers, "").ok());
    for(int fault = 0; fault < 5; ++fault)
    {
        auto bad = tiers;
        if(fault == 0)
            bad[1]["unknown"] = 1;
        if(fault == 1)
            bad[1]["kind"] = "s3";
        if(fault == 2)
            bad[1]["name"] = "local";
        if(fault == 3)
            bad[1]["rank"] = 0;
        if(fault == 4)
            bad[0]["root"] = "/elsewhere";
        EXPECT_FALSE(load(bad).ok()) << fault;
    }
    auto defaults = PlayerConfig::load(std::nullopt, env({}));
    ASSERT_TRUE(defaults.ok());
    EXPECT_TRUE(defaults->tiers.empty());
    EXPECT_TRUE(defaults->deployment_id.empty());
}

TEST(player_config, DefaultsMatchTheSpec)
{
    auto cfg = PlayerConfig::load(std::nullopt, env({}));
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->listen, "0.0.0.0:50054");
    EXPECT_EQ(cfg->batch_size, 1024u);
    EXPECT_EQ(cfg->read_max_events, 262144u);
    EXPECT_EQ(cfg->tail_max_bytes, 64u * 1024 * 1024);
    EXPECT_EQ(cfg->tail_poll_ms, 200u);
    EXPECT_EQ(cfg->keeper_deadline_ms, 2000u);
    EXPECT_TRUE(cfg->archive_root.empty());
    EXPECT_EQ(cfg->manifest_poll_ms, 1000u);
    EXPECT_EQ(cfg->archive_read_timeout_ms, 30000u);
}

TEST(player_config, EnvironmentOverrides)
{
    auto cfg = PlayerConfig::load(std::nullopt,
                                  env({{"CHRONOLOG_PLAYER_LISTEN", "127.0.0.1:1"},
                                       {"CHRONOLOG_PLAYER_BATCH_SIZE", "7"},
                                       {"CHRONOLOG_PLAYER_READ_MAX_EVENTS", "100"},
                                       {"CHRONOLOG_PLAYER_TAIL_MAX_BYTES", "1048576"},
                                       {"CHRONOLOG_PLAYER_ARCHIVE_ROOT", "/archive"},
                                       {"CHRONOLOG_PLAYER_MANIFEST_POLL_MS", "1500"},
                                       {"CHRONOLOG_PLAYER_ARCHIVE_READ_TIMEOUT_MS", "4000"},
                                       {"CHRONOLOG_PLAYER_KEEPER_INTERNAL", "k1=h:1,k2=h:2"}}));
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->listen, "127.0.0.1:1");
    EXPECT_EQ(cfg->batch_size, 7u);
    EXPECT_EQ(cfg->read_max_events, 100u);
    EXPECT_EQ(cfg->tail_max_bytes, 1048576u);
    EXPECT_EQ(cfg->archive_root, "/archive");
    EXPECT_EQ(cfg->manifest_poll_ms, 1500u);
    EXPECT_EQ(cfg->archive_read_timeout_ms, 4000u);
    EXPECT_EQ(cfg->keeperInternal({"k2", "x:9"}), "h:2");
}

TEST(player_config, LogLevelDefaultsToInfoAndRejectsUnknownLevels)
{
    auto defaults = PlayerConfig::load(std::nullopt, env({}));
    ASSERT_TRUE(defaults.ok());
    EXPECT_EQ(defaults->log_level, "info");
    auto overridden = PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_LOG_LEVEL", "warning"}}));
    ASSERT_TRUE(overridden.ok());
    EXPECT_EQ(overridden->log_level, "warning");
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_LOG_LEVEL", "debug"}})).ok());
}

TEST(player_config, KeeperInternalAddressFallsBackToTheEndpointHostPlusSuffix)
{
    auto cfg = PlayerConfig::load(std::nullopt, env({}));
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->keeperInternal({"k", "chrono-keeper:50052"}), "chrono-keeper:50062");
}

TEST(player_config, RejectsBadValues)
{
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_ARCHIVE_READ_TIMEOUT_MS", "0"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_TAIL_MAX_BYTES", "0"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_READ_MAX_EVENTS", "0"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_MANIFEST_POLL_MS", "0"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_BATCH_SIZE", "0"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_TAIL_POLL_MS", "x"}})).ok());
    EXPECT_FALSE(PlayerConfig::load(std::nullopt, env({{"CHRONOLOG_PLAYER_KEEPER_INTERNAL", "nopair"}})).ok());
}

} // namespace
} // namespace chronolog::player
