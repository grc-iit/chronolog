#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>

#include "KeeperConfig.h"
#include "membership/ConfigMembership.h"

namespace chronolog::keeper
{
namespace
{

KeeperConfig::Getenv Env(std::map<std::string, std::string> values)
{
    return [values = std::move(values)](const char* name) -> const char*
    {
        auto it = values.find(name);
        return it == values.end() ? nullptr : it->second.c_str();
    };
}

std::string WriteFile(const std::string& text)
{
    auto path = std::filesystem::temp_directory_path() / ("keeper-config-test-" + std::to_string(::getpid()) + ".json");
    std::ofstream(path) << text;
    return path.string();
}

} // namespace

TEST(KeeperConfig, DefaultsAreValid)
{
    auto cfg = KeeperConfig::load(std::nullopt, Env({}));
    ASSERT_TRUE(cfg.ok()) << cfg.status();
    EXPECT_EQ(cfg->listen, "0.0.0.0:50052");
    EXPECT_EQ(cfg->payload_max_bytes, 1048576u);
    EXPECT_EQ(cfg->dedupe_window, 65536u);
    EXPECT_GE(cfg->effectiveWorkerThreads(), 1u);
}

TEST(KeeperConfig, EnvironmentOverridesFile)
{
    auto path = WriteFile(R"({"process_id":"keeper-9","payload_max_bytes":10,
        "static_routes":[{"story_id":1,"epoch":3,"keepers":[{"process_id":"keeper-9","endpoint":"k:1"}]}],
        "static_writers":[{"story_id":1,"writer_id":2,"incarnation":3}]})");
    auto cfg = KeeperConfig::load(path, Env({{"CHRONOLOG_KEEPER_PAYLOAD_MAX_BYTES", "20"}}));
    std::filesystem::remove(path);
    ASSERT_TRUE(cfg.ok()) << cfg.status();
    EXPECT_EQ(cfg->process_id, "keeper-9");
    EXPECT_EQ(cfg->payload_max_bytes, 20u);
    ASSERT_EQ(cfg->static_routes.size(), 1u);
    EXPECT_EQ(cfg->static_routes[0].route.epoch, 3u);
    ASSERT_EQ(cfg->static_writers.size(), 1u);
}

TEST(KeeperConfig, RejectsUnknownKeysAndWildcardInternalBind)
{
    auto path = WriteFile(R"({"nope":1})");
    EXPECT_FALSE(KeeperConfig::load(path, Env({})).ok());
    std::filesystem::remove(path);
    auto wildcard = KeeperConfig::load(std::nullopt, Env({{"CHRONOLOG_KEEPER_INTERNAL_LISTEN", "0.0.0.0:50062"}}));
    EXPECT_EQ(wildcard.status().code(), absl::StatusCode::kFailedPrecondition);
    auto allowed = KeeperConfig::load(std::nullopt,
                                      Env({{"CHRONOLOG_KEEPER_INTERNAL_LISTEN", "0.0.0.0:50062"},
                                           {"CHRONOLOG_KEEPER_INSECURE_BIND_ALL", "true"}}));
    EXPECT_TRUE(allowed.ok());
    EXPECT_FALSE(KeeperConfig::load(std::nullopt, Env({{"CHRONOLOG_KEEPER_DEDUPE_WINDOW", "-1"}})).ok());
}

TEST(KeeperConfig, ChunkEventLimitDefaultsAndOverridesAreBounded)
{
    auto defaults = KeeperConfig::load(std::nullopt, Env({}));
    ASSERT_TRUE(defaults.ok());
    EXPECT_EQ(defaults->chunk_max_events, 65536u);
    auto path = WriteFile(R"({"chunk_max_events":7})");
    auto file = KeeperConfig::load(path, Env({}));
    ASSERT_TRUE(file.ok());
    EXPECT_EQ(file->chunk_max_events, 7u);
    auto overridden = KeeperConfig::load(path, Env({{"CHRONOLOG_KEEPER_CHUNK_MAX_EVENTS", "2"}}));
    std::filesystem::remove(path);
    ASSERT_TRUE(overridden.ok());
    EXPECT_EQ(overridden->chunk_max_events, 2u);
    for(const auto* invalid: {"0", "65537"})
        EXPECT_FALSE(KeeperConfig::load(std::nullopt, Env({{"CHRONOLOG_KEEPER_CHUNK_MAX_EVENTS", invalid}})).ok());
}

TEST(KeeperConfig, WalRotationAndShutdownSettings)
{
    auto defaults = KeeperConfig::load(std::nullopt, Env({}));
    ASSERT_TRUE(defaults.ok());
    EXPECT_EQ(defaults->wal_segment_bytes, 64u << 20);
    EXPECT_EQ(defaults->shutdown_confirm_timeout_secs, 150u);
    auto path = WriteFile(R"({"wal_segment_bytes":1024,"shutdown_confirm_timeout_secs":5})");
    auto file = KeeperConfig::load(path, Env({}));
    ASSERT_TRUE(file.ok());
    EXPECT_EQ(file->wal_segment_bytes, 1024u);
    EXPECT_EQ(file->shutdown_confirm_timeout_secs, 5u);
    auto overridden = KeeperConfig::load(path,
                                         Env({{"CHRONOLOG_KEEPER_WAL_SEGMENT_BYTES", "2048"},
                                              {"CHRONOLOG_KEEPER_SHUTDOWN_CONFIRM_TIMEOUT_SECS", "0"}}));
    std::filesystem::remove(path);
    ASSERT_TRUE(overridden.ok());
    EXPECT_EQ(overridden->wal_segment_bytes, 2048u);
    EXPECT_EQ(overridden->shutdown_confirm_timeout_secs, 0u);
    EXPECT_FALSE(KeeperConfig::load(std::nullopt, Env({{"CHRONOLOG_KEEPER_WAL_SEGMENT_BYTES", "0"}})).ok());
}

TEST(KeeperConfig, LogLevelDefaultsToInfoAndRejectsUnknownLevels)
{
    auto defaults = KeeperConfig::load(std::nullopt, Env({}));
    ASSERT_TRUE(defaults.ok());
    EXPECT_EQ(defaults->log_level, "info");
    auto overridden = KeeperConfig::load(std::nullopt, Env({{"CHRONOLOG_KEEPER_LOG_LEVEL", "warning"}}));
    ASSERT_TRUE(overridden.ok());
    EXPECT_EQ(overridden->log_level, "warning");
    auto path = WriteFile(R"({"log_level":"error"})");
    auto file = KeeperConfig::load(path, Env({}));
    std::filesystem::remove(path);
    ASSERT_TRUE(file.ok());
    EXPECT_EQ(file->log_level, "error");
    EXPECT_FALSE(KeeperConfig::load(std::nullopt, Env({{"CHRONOLOG_KEEPER_LOG_LEVEL", "debug"}})).ok());
}

TEST(ConfigMembership, SeedsAndReplacesRoutes)
{
    ConfigMembership membership({StaticRoute{1, Route{3, {{"k", "k:1"}}, "g", "p"}}});
    EXPECT_TRUE(membership.validateEpoch(1, 3).ok());
    EXPECT_EQ(membership.validateEpoch(1, 2).code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(membership.validateEpoch(2, 1).code(), absl::StatusCode::kNotFound);
    membership.setRoute(1, Route{4, {{"k", "k:1"}}, "g", "p"});
    EXPECT_TRUE(membership.validateEpoch(1, 4).ok());
    auto route = membership.route(1);
    ASSERT_TRUE(route.ok());
    EXPECT_EQ(route->epoch, 4u);
}

TEST(ConfigMembership, LearnsNewStoriesOnCacheMiss)
{
    unsigned lookups = 0;
    ConfigMembership membership({},
                                [&lookups](StoryId id) -> absl::StatusOr<Route>
                                {
                                    ++lookups;
                                    if(id != 9)
                                        return absl::NotFoundError("unknown story");
                                    return Route{3, {{"k", "k:1"}}, "g", "p"};
                                });
    EXPECT_TRUE(membership.validateEpoch(9, 3).ok());
    EXPECT_EQ(membership.validateEpoch(9, 2).code(), absl::StatusCode::kFailedPrecondition);
    ASSERT_TRUE(membership.route(9).ok());
    EXPECT_EQ(lookups, 1u);
    EXPECT_EQ(membership.route(10).status().code(), absl::StatusCode::kNotFound);
    EXPECT_EQ(lookups, 2u);
    membership.setRoute(9, Route{4, {{"k", "k:1"}}, "g", "p"});
    EXPECT_TRUE(membership.validateEpoch(9, 4).ok());
}

} // namespace chronolog::keeper
