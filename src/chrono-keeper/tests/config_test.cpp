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

} // namespace chronolog::keeper
