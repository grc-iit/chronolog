#include "TestSupport.h"
#include "dynamic/MembershipState.h"
#include "raft/RaftMetadataStore.h"
#include "clock/FakeClock.h"
#include "journal/RamJournal.h"
#include "membership/ConfigMembership.h"
#include <gtest/gtest.h>
#include <chrono>
#include <map>
#include <optional>
#include <thread>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>

namespace chronolog::floor_contract
{
namespace wire = internal::v1;
class MembershipContract: public ::testing::TestWithParam<bool>
{
protected:
    visor::testing::TempDir dir;
    std::unique_ptr<visor::SqliteMetadataStore> sqlite;
    std::unique_ptr<visor::RaftMetadataStore> raft;
    MetadataStore* catalog{};
    visor::SqliteMetadataStore* applied{};
    void SetUp() override
    {
        if(GetParam())
        {
            absl::Status status;
            for(int attempt = 0; attempt < 8; ++attempt)
            {
                int fd = socket(AF_INET, SOCK_STREAM, 0);
                ASSERT_GE(fd, 0);
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
                socklen_t size = sizeof(address);
                ASSERT_EQ(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size), 0);
                const auto endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
                close(fd);
                auto opened = visor::RaftMetadataStore::open((dir.path() / ("raft" + std::to_string(attempt))).string(),
                                                             visor::testing::twoKeeperTopology(),
                                                             {1, endpoint, {{1, endpoint, endpoint, endpoint}}});
                status = opened.status();
                if(opened.ok())
                {
                    raft = std::move(*opened);
                    break;
                }
                if(status.message().find(" in use") == std::string_view::npos)
                    break;
            }
            ASSERT_TRUE(status.ok()) << status;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while(!raft->leaderLease() && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ASSERT_TRUE(raft->leaderLease());
            catalog = raft.get();
            applied = &raft->appliedStore();
        }
        else
        {
            auto opened = visor::SqliteMetadataStore::open((dir.path() / "sqlite").string(),
                                                           visor::testing::twoKeeperTopology());
            ASSERT_TRUE(opened.ok());
            sqlite = std::move(*opened);
            catalog = applied = sqlite.get();
        }
        ASSERT_TRUE(catalog->createChronicle("c").ok());
        ASSERT_TRUE(catalog->createStory("c", "s").ok());
        for(const auto& id: {"keeper-a", "keeper-b"})
        {
            wire::MembershipCommand q;
            auto* p = q.mutable_register_()->mutable_process();
            p->set_process_id(id);
            p->set_instance(id);
            p->set_endpoint(std::string(id) + ":50052");
            p->set_role(wire::PROCESS_ROLE_KEEPER);
            ASSERT_EQ(call<wire::RegisterResponse>(q).status().code(), 0);
            q.Clear();
            auto* e = q.mutable_extend();
            e->set_process_id(id);
            e->set_instance(id);
            e->set_applied_route_revision(10000);
            e->set_realtime_ns(100);
            e->mutable_wanted_hlc()->set_physical_ns(100);
            ASSERT_EQ(call<wire::ExtendCeilingResponse>(q).status().code(), 0);
        }
    }
    template <class Response>
    Response call(const wire::MembershipCommand& q)
    {
        wire::CatalogCommand c;
        *c.mutable_membership() = q;
        auto result = raft ? raft->propose(c)
                           : applied->applyRaft(applied->appliedIndex().value_or(0) + 1,
                                                [&] { return visor::dynamic::apply(*applied, q); });
        if(!result.ok())
            throw std::runtime_error(result.status().ToString());
        Response response;
        if(!response.ParseFromString(*result))
            throw std::runtime_error("bad response");
        return response;
    }
    wire::RouteUpdate route() { return applied->membershipRouteUpdate(1).value(); }
    Acquisition releasedWriter()
    {
        auto writer = catalog->acquire(1, "released").value();
        if(!catalog->release(1, writer.writer_id, writer.incarnation).ok())
            throw std::runtime_error("release failed");
        return writer;
    }
    void join(const std::string& id)
    {
        wire::MembershipCommand q;
        auto* p = q.mutable_register_()->mutable_process();
        p->set_process_id(id);
        p->set_instance(id);
        p->set_endpoint(id + ":50052");
        p->set_role(wire::PROCESS_ROLE_KEEPER);
        ASSERT_EQ(call<wire::RegisterResponse>(q).status().code(), 0);
        q.Clear();
        q.mutable_join()->set_process_id(id);
        ASSERT_EQ(call<wire::MembershipResponse>(q).status().code(), 0);
        q.Clear();
        auto* e = q.mutable_extend();
        e->set_process_id(id);
        e->set_instance(id);
        e->set_applied_route_revision(10000);
        e->set_realtime_ns(100);
        e->mutable_wanted_hlc()->set_physical_ns(100);
        ASSERT_EQ(call<wire::ExtendCeilingResponse>(q).status().code(), 0);
    }
    void remove(const std::string& id, bool abandon = false)
    {
        wire::MembershipCommand q;
        (abandon ? q.mutable_abandon() : q.mutable_drain())->set_process_id(id);
        ASSERT_EQ(call<wire::MembershipResponse>(q).status().code(), 0);
    }
};
#include "membership_floor_contract_test.inc"
INSTANTIATE_TEST_SUITE_P(Sqlite, MembershipContract, ::testing::Values(false));
INSTANTIATE_TEST_SUITE_P(Raft, MembershipContract, ::testing::Values(true));

// RFC-G section 8 first-assignment preference and section 9 active count over the same SQLite and Raft stores.
class PreferredKeeper: public MembershipContract
{
protected:
    absl::StatusOr<Acquisition>
    acquire(const std::string& identity, std::optional<std::string> hint, bool takeover = false)
    {
        AcquireOptions options;
        options.preferred_keeper_process_id = std::move(hint);
        options.takeover = takeover;
        return catalog->acquire(1, identity, options);
    }
    void expire(const Acquisition& a)
    {
        if(raft)
        {
            wire::CatalogCommand c;
            auto* t = c.mutable_expire_acquisitions()->add_acquisitions();
            t->set_story_id(a.story_id);
            t->set_writer_id(a.writer_id);
            t->set_incarnation(a.incarnation);
            ASSERT_TRUE(raft->propose(c).ok());
        }
        else
            ASSERT_TRUE(applied->expireAcquisitions({{a.story_id, a.writer_id, a.incarnation}}).ok());
        auto rows = applied->acquisitionRows({{a.story_id, a.writer_id, a.incarnation}});
        ASSERT_TRUE(rows.ok());
        ASSERT_EQ(rows->at(0).termination_cause, AcquisitionTerminationCause::Expired);
    }
    void restart()
    {
        sqlite.reset();
        auto opened =
                visor::SqliteMetadataStore::open((dir.path() / "sqlite").string(), visor::testing::twoKeeperTopology());
        ASSERT_TRUE(opened.ok()) << opened.status();
        sqlite = std::move(*opened);
        catalog = applied = sqlite.get();
    }
    void name(const Acquisition& a, const std::string& identity) { identity_of[a.writer_id] = identity; }
    static bool floored(const wire::RouteUpdate& u, const std::string& keeper)
    {
        return std::find(u.observe_floor().begin(), u.observe_floor().end(), keeper) != u.observe_floor().end();
    }
    std::map<uint64_t, std::string> identity_of;
};

TEST_P(PreferredKeeper, PreferredKeeperInRouteIsAssigned)
{
    // writer_id 1 maps to keeper-b, so keeper-a proves the hint placed it.
    const auto first = acquire("w", "keeper-a");
    ASSERT_TRUE(first.ok()) << first.status();
    ASSERT_EQ(first->writer_id % 2, 1u);
    EXPECT_EQ(first->assigned_keeper, (KeeperRef{"keeper-a", "keeper-a:50052"}));
    EXPECT_NE(std::find(first->route.keepers.begin(), first->route.keepers.end(), first->assigned_keeper),
              first->route.keepers.end());
    EXPECT_EQ(first->keeper_preference, KeeperPreferenceResult::Honored);
    const auto absent = acquire("other", std::nullopt);
    ASSERT_TRUE(absent.ok()) << absent.status();
    EXPECT_FALSE(absent->keeper_preference);
    EXPECT_EQ(absent->assigned_keeper, absent->route.keepers[absent->writer_id % 2]);
}

TEST_P(PreferredKeeper, PreferredKeeperOutsideRouteFallsBackAndDiscloses)
{
    const auto stale = acquire("w", "keeper-z");
    ASSERT_TRUE(stale.ok()) << stale.status();
    EXPECT_EQ(stale->assigned_keeper, stale->route.keepers[stale->writer_id % 2]);
    EXPECT_EQ(stale->keeper_preference, KeeperPreferenceResult::NotInRoute);
}

TEST_P(PreferredKeeper, PreferenceCannotMoveAnExistingWriterWithinEpoch)
{
    auto held = acquire("w", "keeper-a");
    ASSERT_TRUE(held.ok()) << held.status();
    ASSERT_EQ(held->assigned_keeper.process_id, "keeper-a");
    // Takeover, release and a joined Keeper named by the hint all keep the surviving owner.
    held = acquire("w", "keeper-b", true);
    ASSERT_TRUE(held.ok()) << held.status();
    EXPECT_EQ(held->assigned_keeper.process_id, "keeper-a");
    EXPECT_EQ(held->keeper_preference, KeeperPreferenceResult::Retained);
    ASSERT_TRUE(catalog->release(1, held->writer_id, held->incarnation).ok());
    join("keeper-c");
    for(const auto& [hint, result]: {std::pair{std::string("keeper-c"), KeeperPreferenceResult::Retained},
                                     std::pair{std::string("keeper-a"), KeeperPreferenceResult::Retained},
                                     std::pair{std::string("keeper-z"), KeeperPreferenceResult::NotInRoute}})
    {
        const auto again = acquire("w", hint);
        ASSERT_TRUE(again.ok()) << again.status();
        EXPECT_EQ(again->assigned_keeper, (KeeperRef{"keeper-a", "keeper-a:50052"}));
        EXPECT_EQ(again->keeper_preference, result) << hint;
        ASSERT_TRUE(catalog->release(1, again->writer_id, again->incarnation).ok());
    }
    // A writer first placed without a hint keeps its modulo owner when a later hint names another Keeper.
    const auto plain = acquire("plain", std::nullopt).value();
    ASSERT_TRUE(catalog->release(1, plain.writer_id, plain.incarnation).ok());
    const auto other = plain.assigned_keeper.process_id == "keeper-a" ? "keeper-b" : "keeper-a";
    const auto hinted = acquire("plain", other);
    ASSERT_TRUE(hinted.ok()) << hinted.status();
    EXPECT_EQ(hinted->assigned_keeper.process_id, plain.assigned_keeper.process_id);
    EXPECT_EQ(hinted->keeper_preference, KeeperPreferenceResult::Retained);
}

TEST_P(PreferredKeeper, PreferredAffinitySurvivesReleaseExpiryAndRestart)
{
    const auto first = acquire("w", "keeper-a").value();
    expire(first);
    auto again = acquire("w", "keeper-b");
    ASSERT_TRUE(again.ok()) << again.status();
    EXPECT_EQ(again->assigned_keeper.process_id, "keeper-a");
    EXPECT_EQ(again->keeper_preference, KeeperPreferenceResult::Retained);
    EXPECT_EQ(again->incarnation, first.incarnation + 1);
    ASSERT_TRUE(catalog->release(1, again->writer_id, again->incarnation).ok());
    if(!raft)
        restart();
    again = acquire("w", "keeper-b");
    ASSERT_TRUE(again.ok()) << again.status();
    EXPECT_EQ(again->assigned_keeper.process_id, "keeper-a");
    EXPECT_EQ(again->keeper_preference, KeeperPreferenceResult::Retained);
}

TEST_P(PreferredKeeper, RemovedOwnerRemapsByWriterIdWithFenceAndFloor)
{
    join("keeper-c");
    // One writer released and one expired before its preferred Keeper is drained.
    const auto released = acquire("released", "keeper-a").value();
    const auto expired = acquire("expired", "keeper-a").value();
    ASSERT_EQ(released.keeper_preference, KeeperPreferenceResult::Honored);
    ASSERT_EQ(expired.keeper_preference, KeeperPreferenceResult::Honored);
    ASSERT_TRUE(catalog->release(1, released.writer_id, released.incarnation).ok());
    expire(expired);
    remove("keeper-a");
    // The old owner stays live and unfenced until the waiter confirms, expiry included.
    std::vector<std::pair<std::string, uint64_t>> waited;
    bool confirm = false;
    applied->setOwnerFence(
            [&](const KeeperRef& keeper, uint64_t revision)
            {
                waited.emplace_back(keeper.process_id, revision);
                return confirm;
            });
    const auto update = route();
    ASSERT_EQ(update.route().keepers_size(), 2);
    for(const auto& [writer, identity]:
        {std::pair{released, std::string("released")}, std::pair{expired, std::string("expired")}})
    {
        const auto& target = update.route().keepers(static_cast<int>(writer.writer_id % 2)).process_id();
        const auto bystander = target == "keeper-b" ? "keeper-c" : "keeper-b";
        EXPECT_TRUE(floored(update, target)) << identity;
        // An eligible hint cannot replace the precomputed target; a stale one is disclosed.
        const auto hint = writer.writer_id == released.writer_id ? std::string(bystander) : "keeper-a";
        confirm = false;
        EXPECT_TRUE(absl::IsUnavailable(acquire(identity, hint).status())) << identity;
        auto prior = applied->acquisitionRows({{1, writer.writer_id, writer.incarnation}});
        ASSERT_TRUE(prior.ok());
        EXPECT_EQ(prior->at(0).state, visor::AcquisitionState::Released);
        EXPECT_EQ(prior->at(0).assigned_keeper.process_id, "keeper-a");
        ASSERT_FALSE(waited.empty());
        EXPECT_EQ(waited.back(), (std::pair{std::string("keeper-a"), prior->at(0).revision}));
        confirm = true;
        const auto next = acquire(identity, hint);
        ASSERT_TRUE(next.ok()) << next.status();
        EXPECT_EQ(next->assigned_keeper.process_id, target) << identity;
        EXPECT_EQ(next->incarnation, writer.incarnation + 1);
        EXPECT_EQ(next->keeper_preference,
                  writer.writer_id == released.writer_id ? KeeperPreferenceResult::Retained
                                                         : KeeperPreferenceResult::NotInRoute);
    }
    applied->setOwnerFence({});
}

INSTANTIATE_TEST_SUITE_P(Sqlite, PreferredKeeper, ::testing::Values(false));
INSTANTIATE_TEST_SUITE_P(Raft, PreferredKeeper, ::testing::Values(true));

class CatalogMetricsTest: public PreferredKeeper
{
protected:
    std::map<std::string, uint64_t> counts()
    {
        auto snapshot = applied->snapshotAcquisitions();
        EXPECT_TRUE(snapshot.ok());
        // The committed rows themselves, read independently of the snapshot.
        std::map<std::string, uint64_t> rows;
        for(const auto& [writer, identity]: identity_of)
        {
            auto row = applied->currentAcquisition(1, identity);
            EXPECT_TRUE(row.ok() && *row);
            if((*row)->state == visor::AcquisitionState::Acquired)
                ++rows[(*row)->assigned_keeper.process_id];
        }
        auto counted = visor::activeAcquisitionsPerKeeper(*snapshot);
        EXPECT_EQ(counted, rows);
        return counted;
    }
    Acquisition held(const std::string& identity, const std::string& hint)
    {
        auto a = acquire(identity, hint).value();
        name(a, identity);
        return a;
    }
};

TEST_P(CatalogMetricsTest, PerKeeperAcquisitionCountTracksCommittedActiveRows)
{
    using Counts = std::map<std::string, uint64_t>;
    EXPECT_EQ(counts(), Counts{});
    const auto a1 = held("a1", "keeper-a");
    held("a2", "keeper-a");
    const auto b1 = held("b1", "keeper-b");
    EXPECT_EQ(counts(), (Counts{{"keeper-a", 2}, {"keeper-b", 1}}));
    ASSERT_TRUE(catalog->release(1, a1.writer_id, a1.incarnation).ok());
    EXPECT_EQ(counts(), (Counts{{"keeper-a", 1}, {"keeper-b", 1}}));
    expire(b1);
    EXPECT_EQ(counts(), (Counts{{"keeper-a", 1}}));
    // Retained reacquisition counts once, on its surviving owner.
    held("b1", "keeper-a");
    EXPECT_EQ(counts(), (Counts{{"keeper-a", 1}, {"keeper-b", 1}}));
    if(!raft)
    {
        restart();
        EXPECT_EQ(counts(), (Counts{{"keeper-a", 1}, {"keeper-b", 1}}));
    }
    // Removal fences the removed Keeper's active rows, so they leave the count.
    remove("keeper-a");
    EXPECT_EQ(counts(), (Counts{{"keeper-b", 1}}));
}

INSTANTIATE_TEST_SUITE_P(Sqlite, CatalogMetricsTest, ::testing::Values(false));
INSTANTIATE_TEST_SUITE_P(Raft, CatalogMetricsTest, ::testing::Values(true));
} // namespace chronolog::floor_contract
