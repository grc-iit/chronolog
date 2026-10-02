// Instantiates MembershipContract for StaticRouteMembership and covers the parts of
// its behavior the generic suite cannot see: heartbeat liveness, applied revisions and
// the Release fence wait.
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "TestSupport.h"
#include "VisorConfig.h"
#include "catalog/SqliteMetadataStore.h"
#include "membership/StaticRouteMembership.h"
#include "membership_contract_test.cpp"

namespace chronolog::contract
{
namespace
{

using visor::StaticRouteMembership;
using visor::testing::twoKeeperTopology;
using namespace std::chrono_literals;

std::unique_ptr<StaticRouteMembership>
make(Epoch epoch, std::function<StaticRouteMembership::TimePoint()> now = &std::chrono::steady_clock::now)
{
    return std::make_unique<StaticRouteMembership>(
            twoKeeperTopology(),
            epoch,
            [](StoryId id) { return id == 1; },
            15s,
            std::move(now));
}

MembershipFactory staticFactory(bool raft)
{
    return [raft]
    {
        auto dir = std::make_shared<visor::testing::TempDir>();
        auto opened = visor::SqliteMetadataStore::open((dir->path() / "catalog").string(), twoKeeperTopology());
        if(!opened.ok())
            throw std::runtime_error(opened.status().ToString());
        auto store = std::shared_ptr<visor::SqliteMetadataStore>(std::move(*opened));
        if(!store->registerStaticPolicy("keeper-a", 1).ok() || !store->registerStaticPolicy("keeper-b", 1).ok() ||
           !store->createChronicle("c").ok() || !store->createStory("c", "s").ok() ||
           !store->compareAndSetEpoch(1, 1, 7).ok())
            throw std::runtime_error("setup failed");
        auto harness = std::make_unique<MembershipHarness>();
        harness->sut = std::make_unique<StaticRouteMembership>(
                twoKeeperTopology(),
                7,
                [store](StoryId id)
                {
                    auto story = store->getStory(id);
                    return story.ok() && !story->tombstoned;
                },
                15s);
        harness->staticEpoch = [] { return make(1); };
        harness->destroyStory = [store, raft]
        {
            if(!raft)
                return store->destroyStory(1);
            absl::Status status;
            auto applied = store->applyRaft(store->appliedIndex().value_or(0) + 1,
                                            [&]
                                            {
                                                status = store->destroyStory(1);
                                                return status.ToString();
                                            });
            return applied.ok() ? status : applied.status();
        };
        harness->storyEpoch = [store] { return store->getStory(1)->epoch; };
        harness->revision = [store] { return store->membershipRevision().value_or(0); };
        harness->deltasSince = [store](uint64_t cursor)
        {
            std::vector<MembershipHarness::RouteDelta> out;
            auto changes = store->membershipRouteChanges(cursor);
            if(!changes.ok())
                throw std::runtime_error(changes.status().ToString());
            for(const auto& r: changes->route_history())
                out.push_back({r.story_id(), r.revision(), r.tombstoned(), r.route().epoch(), r.physical_policy()});
            return out;
        };
        harness->createPolicyStory = [store, dir]() -> absl::StatusOr<StoryId>
        {
            auto story = store->createStory("c", "policy");
            if(!story.ok())
                return story.status();
            if(!store->membershipRouteUpdate(1)->physical_policy() ||
               !store->membershipRouteUpdate(story->id)->physical_policy())
                return absl::InternalError("policy setup failed");
            return story->id;
        };
        harness->clearPhysicalPolicy = [store, raft](const std::vector<StoryId>& stories)
        {
            if(!raft)
                return store->clearPhysicalPolicy(stories);
            absl::Status status;
            auto applied = store->applyRaft(store->appliedIndex().value_or(0) + 1,
                                            [&]
                                            {
                                                status = store->clearPhysicalPolicy(stories);
                                                return status.ToString();
                                            });
            return applied.ok() ? status : applied.status();
        };
        return harness;
    };
}

std::string paramName(const ::testing::TestParamInfo<MembershipFactory>&) { return "Default"; }

INSTANTIATE_TEST_SUITE_P(Sqlite, MembershipContract, ::testing::Values(staticFactory(false)), paramName);
INSTANTIATE_TEST_SUITE_P(Raft, MembershipContract, ::testing::Values(staticFactory(true)), paramName);

TEST(sqlite_route_history, FailedPolicyHistoryWriteRollsBackEveryRouteAndRevision)
{
    visor::testing::TempDir dir;
    const auto path = (dir.path() / "catalog").string();
    auto opened = visor::SqliteMetadataStore::open(path, twoKeeperTopology());
    ASSERT_TRUE(opened.ok());
    auto store = std::move(*opened);
    ASSERT_TRUE(store->registerStaticPolicy("keeper-a", 1).ok());
    ASSERT_TRUE(store->registerStaticPolicy("keeper-b", 1).ok());
    ASSERT_TRUE(store->createChronicle("c").ok());
    ASSERT_TRUE(store->createStory("c", "a").ok());
    ASSERT_TRUE(store->createStory("c", "b").ok());
    const auto cursor = store->membershipRevision().value_or(0);
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    EXPECT_EQ(sqlite3_exec(db,
                           "CREATE TRIGGER refuse_policy_history BEFORE INSERT ON membership_history WHEN "
                           "NEW.story_id=2 BEGIN SELECT RAISE(ABORT,'history write refused'); END",
                           nullptr,
                           nullptr,
                           nullptr),
              SQLITE_OK);
    sqlite3_close(db);
    EXPECT_FALSE(store->clearPhysicalPolicy({1, 2}).ok());
    EXPECT_EQ(store->membershipRevision().value_or(0), cursor);
    EXPECT_TRUE(store->membershipRouteUpdate(1)->physical_policy());
    EXPECT_TRUE(store->membershipRouteUpdate(2)->physical_policy());
    EXPECT_EQ(store->membershipRouteChanges(cursor)->route_history_size(), 0);
}

TEST(static_route_membership, AssignsGraphersByStoryAcrossRepeatedRoutes)
{
    auto topology = twoKeeperTopology();
    topology.graphers = {"grapher-a:50053", "grapher-b:50053"};
    StaticRouteMembership membership(topology, 1, [](StoryId) { return true; }, 15s);
    for(StoryId story = 1; story <= 10; ++story)
    {
        auto first = membership.route(story);
        auto repeated = membership.route(story);
        ASSERT_TRUE(first.ok());
        ASSERT_TRUE(repeated.ok());
        EXPECT_EQ(first->grapher, topology.graphers[story % 2]);
        EXPECT_EQ(repeated->grapher, first->grapher);
        EXPECT_EQ(topology.routeFor(2, story).grapher, first->grapher);
    }
    topology.graphers.clear();
    EXPECT_EQ(topology.routeFor(1, 7).grapher, topology.grapher);
}

TEST(static_route_membership, GraphersEnvironmentOverridesJsonAndRejectsEmptyEndpoints)
{
    visor::testing::TempDir dir;
    const auto path = (dir.path() / "visor.json").string();
    {
        std::ofstream file(path);
        file << R"({"graphers":["a:50053","b:50053"]})";
    }
    auto cfg = visor::VisorConfig::load(path, [](const char*) -> const char* { return nullptr; });
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->graphers, (std::vector<std::string>{"a:50053", "b:50053"}));
    cfg = visor::VisorConfig::load(path,
                                   [](const char* key) -> const char* {
                                       return std::string(key) == "CHRONOLOG_VISOR_GRAPHERS" ? "c:50053,d:50053"
                                                                                             : nullptr;
                                   });
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->graphers, (std::vector<std::string>{"c:50053", "d:50053"}));
    {
        std::ofstream file(path);
        file << R"({"graphers":[""]})";
    }
    EXPECT_FALSE(visor::VisorConfig::load(path, [](const char*) -> const char* { return nullptr; }).ok());
}

TEST(static_route_membership, LogLevelDefaultsToInfoTakesEnvironmentAndRejectsUnknownValues)
{
    auto none = [](const char*) -> const char* { return nullptr; };
    auto cfg = visor::VisorConfig::load(std::nullopt, none);
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->log_level, "info");
    cfg = visor::VisorConfig::load(std::nullopt,
                                   [](const char* key) -> const char*
                                   { return std::string(key) == "CHRONOLOG_VISOR_LOG_LEVEL" ? "warning" : nullptr; });
    ASSERT_TRUE(cfg.ok());
    EXPECT_EQ(cfg->log_level, "warning");
    visor::testing::TempDir dir;
    const auto path = (dir.path() / "visor.json").string();
    {
        std::ofstream file(path);
        file << R"({"log_level":"debug"})";
    }
    EXPECT_FALSE(visor::VisorConfig::load(path, none).ok());
}

TEST(static_route_membership, AppliedRevisionNeverLowersAndResetsOnNewInstance)
{
    auto membership = make(1);
    const Process keeper{"keeper-1", "i1", "keeper-a:50052", ProcessRole::Keeper};
    ASSERT_TRUE(membership->registerProcess(keeper).ok());
    ASSERT_TRUE(membership->heartbeat("keeper-1", "i1", 5).ok());
    EXPECT_TRUE(membership->waitApplied("keeper-1", 5, 0ms));
    ASSERT_TRUE(membership->heartbeat("keeper-1", "i1", 3).ok());
    EXPECT_TRUE(membership->waitApplied("keeper-1", 5, 0ms));
    EXPECT_FALSE(membership->waitApplied("keeper-1", 6, 0ms));
    EXPECT_FALSE(membership->waitApplied("unregistered", 1, 0ms));

    Process restarted = keeper;
    restarted.instance = "i2";
    ASSERT_TRUE(membership->registerProcess(restarted).ok());
    EXPECT_FALSE(membership->waitApplied("keeper-1", 1, 0ms));
    EXPECT_FALSE(membership->heartbeat("keeper-1", "i1", 9).ok());
    EXPECT_FALSE(membership->waitApplied("keeper-1", 9, 0ms));
}

TEST(static_route_membership, WaitAppliedWakesWhenAHeartbeatArrives)
{
    auto membership = make(1);
    ASSERT_TRUE(membership->registerProcess(Process{"keeper-1", "i1", "keeper-a:50052", ProcessRole::Keeper}).ok());
    std::atomic<bool> result{false};
    std::thread waiter([&] { result = membership->waitApplied("keeper-1", 4, 10s); });
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(membership->heartbeat("keeper-1", "i1", 4).ok());
    waiter.join();
    EXPECT_TRUE(result.load());
}

TEST(static_route_membership, WaitAppliedTimesOutWithoutConfirmation)
{
    auto membership = make(1);
    ASSERT_TRUE(membership->registerProcess(Process{"keeper-1", "i1", "keeper-a:50052", ProcessRole::Keeper}).ok());
    EXPECT_FALSE(membership->waitApplied("keeper-1", 1, 30ms));
}

TEST(static_route_membership, LivenessFollowsHeartbeatTimeout)
{
    StaticRouteMembership::TimePoint now{};
    auto membership = make(1, [&now] { return now; });
    ASSERT_TRUE(membership->registerProcess(Process{"keeper-1", "i1", "keeper-a:50052", ProcessRole::Keeper}).ok());
    EXPECT_TRUE(membership->alive("keeper-1"));
    now += 14s;
    EXPECT_TRUE(membership->alive("keeper-1"));
    now += 2s;
    EXPECT_FALSE(membership->alive("keeper-1"));
    ASSERT_TRUE(membership->heartbeat("keeper-1", "i1").ok());
    EXPECT_TRUE(membership->alive("keeper-1"));
    EXPECT_FALSE(membership->alive("never-registered"));
}

TEST(static_route_membership, RejectsMalformedRegistration)
{
    auto membership = make(1);
    EXPECT_EQ(membership->registerProcess(Process{"", "i", "e", ProcessRole::Keeper}).code(),
              absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(membership->heartbeat("", "i").code(), absl::StatusCode::kInvalidArgument);
    EXPECT_EQ(membership->heartbeat("unknown", "i").code(), absl::StatusCode::kNotFound);
}

} // namespace
} // namespace chronolog::contract
