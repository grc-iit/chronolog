#include "TestSupport.h"
#include <chrono>
#include <iostream>
#include "dynamic/DynamicMembership.h"
#include "membership_contract_test.cpp"
namespace chronolog::contract
{
INSTANTIATE_TEST_SUITE_P(
        Dynamic,
        MembershipContract,
        ::testing::Values(MembershipFactory(
                []
                {
                    auto dir = std::make_shared<visor::testing::TempDir>();
                    auto opened = visor::SqliteMetadataStore::open((dir->path() / "catalog").string(),
                                                                   visor::testing::twoKeeperTopology());
                    if(!opened.ok())
                        throw std::runtime_error(opened.status().ToString());
                    auto store = std::shared_ptr<visor::SqliteMetadataStore>(std::move(*opened));
                    if(!store->createChronicle("c").ok() || !store->createStory("c", "s").ok() ||
                       !store->compareAndSetEpoch(1, 1, 7).ok())
                        throw std::runtime_error("setup failed");
                    auto harness = std::make_unique<MembershipHarness>();
                    auto submit = [dir, store](const internal::v1::CatalogCommand& c)
                    {
                        return store->applyRaft(store->appliedIndex().value_or(0) + 1,
                                                [&] { return visor::dynamic::apply(*store, c.membership()); });
                    };
                    harness->sut = std::make_unique<visor::DynamicMembership>(*store, submit);
                    auto send = [submit]<class Response>(const internal::v1::CatalogCommand& c)
                    {
                        auto result = submit(c);
                        if(!result.ok())
                            return result.status();
                        Response r;
                        if(!r.ParseFromString(*result))
                            return absl::InternalError("invalid response");
                        return absl::Status(static_cast<absl::StatusCode>(r.status().code()), r.status().message());
                    };
                    harness->registerPolicy = [send](uint64_t version)
                    {
                        internal::v1::CatalogCommand c;
                        auto* q = c.mutable_membership()->mutable_register_();
                        q->set_policy_version(version);
                        auto* p = q->mutable_process();
                        p->set_process_id("policy-test");
                        p->set_instance("instance");
                        p->set_endpoint("localhost:1");
                        p->set_role(internal::v1::PROCESS_ROLE_KEEPER);
                        return send.template operator()<internal::v1::RegisterResponse>(c);
                    };
                    harness->grantCeiling = [send](std::string id, std::string instance)
                    {
                        internal::v1::CatalogCommand c;
                        auto* q = c.mutable_membership()->mutable_extend();
                        q->set_process_id(id);
                        q->set_instance(instance);
                        q->set_applied_route_revision(10000);
                        q->set_realtime_ns(100);
                        q->mutable_wanted_hlc()->set_physical_ns(100);
                        return send.template operator()<internal::v1::ExtendCeilingResponse>(c);
                    };
                    harness->drainKeeper = [send](std::string id)
                    {
                        internal::v1::CatalogCommand c;
                        c.mutable_membership()->mutable_drain()->set_process_id(id);
                        return send.template operator()<internal::v1::MembershipResponse>(c);
                    };
                    harness->joinKeeper = [send](std::string id)
                    {
                        internal::v1::CatalogCommand c;
                        c.mutable_membership()->mutable_join()->set_process_id(id);
                        return send.template operator()<internal::v1::MembershipResponse>(c);
                    };
                    harness->abandonKeeper = [send](std::string id)
                    {
                        internal::v1::CatalogCommand c;
                        c.mutable_membership()->mutable_abandon()->set_process_id(id);
                        return send.template operator()<internal::v1::MembershipResponse>(c);
                    };
                    harness->reportDrain = [send](std::string id, std::string instance, Epoch epoch, Hlc cut)
                    {
                        internal::v1::CatalogCommand c;
                        auto* q = c.mutable_membership()->mutable_heartbeat();
                        q->set_process_id(id);
                        q->set_instance(instance);
                        auto* f = q->add_story_frontiers();
                        f->set_story_id(1);
                        f->set_drained_instance(instance);
                        f->set_drained_epoch(epoch);
                        f->mutable_sealed_frontier()->set_physical_ns(cut.physical_ns);
                        f->mutable_sealed_frontier()->set_logical(cut.logical);
                        *f->mutable_evicted_below() = f->sealed_frontier();
                        return send.template operator()<internal::v1::HeartbeatResponse>(c);
                    };
                    harness->reportSettlement =
                            [send](std::string id, std::string instance, Hlc coverage, Hlc through, Hlc first)
                    {
                        internal::v1::CatalogCommand c;
                        auto* q = c.mutable_membership()->mutable_heartbeat();
                        q->set_process_id(id);
                        q->set_instance(instance);
                        auto* f = q->add_story_frontiers();
                        f->set_story_id(1);
                        auto* p = f->mutable_settlement();
                        p->set_instance(instance);
                        p->mutable_coverage_start()->set_physical_ns(coverage.physical_ns);
                        p->mutable_coverage_start()->set_logical(coverage.logical);
                        p->mutable_settled_through()->set_physical_ns(through.physical_ns);
                        p->mutable_settled_through()->set_logical(through.logical);
                        p->mutable_first_event()->set_physical_ns(first.physical_ns);
                        p->mutable_first_event()->set_logical(first.logical);
                        return send.template operator()<internal::v1::HeartbeatResponse>(c);
                    };
                    return harness;
                })));
} // namespace chronolog::contract
namespace chronolog::visor
{
namespace wire = internal::v1;
class DynamicMembershipTest: public ::testing::Test
{
protected:
    testing::TempDir dir;
    std::unique_ptr<SqliteMetadataStore> store;
    double lastApplyMs{};
    void SetUp() override
    {
        auto opened = SqliteMetadataStore::open((dir.path() / "catalog").string(), testing::twoKeeperTopology());
        ASSERT_TRUE(opened.ok());
        store = std::move(*opened);
        ASSERT_TRUE(store->createChronicle("c").ok());
        ASSERT_TRUE(store->createStory("c", "s").ok());
    }
    template <class Response>
    Response call(const wire::MembershipCommand& q)
    {
        auto applied = store->applyRaft(
                store->appliedIndex().value_or(0) + 1,
                [&]
                {
                    const auto start = std::chrono::steady_clock::now();
                    auto result = dynamic::apply(*store, q);
                    lastApplyMs =
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    return result;
                });
        if(!applied.ok())
            throw std::runtime_error(applied.status().ToString());
        Response r;
        if(!r.ParseFromString(*applied))
            throw std::runtime_error("invalid response");
        return r;
    }
    wire::RegisterResponse reg(std::string id, std::string instance, std::string recovered = "")
    {
        wire::MembershipCommand q;
        auto* r = q.mutable_register_();
        r->set_recovered_instance(recovered);
        auto* p = r->mutable_process();
        p->set_process_id(id);
        p->set_instance(instance);
        p->set_endpoint(id + ":50052");
        p->set_role(wire::PROCESS_ROLE_KEEPER);
        return call<wire::RegisterResponse>(q);
    }
    wire::ExtendCeilingResponse extend(std::string id,
                                       std::string instance,
                                       uint64_t revision = 10000,
                                       int64_t wanted = 100,
                                       int64_t realtime = 100)
    {
        wire::MembershipCommand q;
        auto* r = q.mutable_extend();
        r->set_process_id(id);
        r->set_instance(instance);
        r->set_applied_route_revision(revision);
        r->mutable_wanted_hlc()->set_physical_ns(wanted);
        r->set_realtime_ns(realtime);
        return call<wire::ExtendCeilingResponse>(q);
    }
    wire::HeartbeatResponse heartbeat(std::string id, std::string instance, const wire::StoryFrontiers& f = {})
    {
        wire::MembershipCommand q;
        auto* r = q.mutable_heartbeat();
        r->set_process_id(id);
        r->set_instance(instance);
        if(f.story_id())
            *r->add_story_frontiers() = f;
        return call<wire::HeartbeatResponse>(q);
    }
    wire::MembershipResponse change(std::string id, int action)
    {
        wire::MembershipCommand q;
        auto* r = action == 0 ? q.mutable_drain() : action == 1 ? q.mutable_join() : q.mutable_abandon();
        r->set_process_id(id);
        return call<wire::MembershipResponse>(q);
    }
    void ready()
    {
        ASSERT_EQ(reg("keeper-a", "a1").status().code(), 0);
        ASSERT_EQ(reg("keeper-b", "b1").status().code(), 0);
        ASSERT_EQ(extend("keeper-a", "a1").status().code(), 0);
        ASSERT_EQ(extend("keeper-b", "b1").status().code(), 0);
    }
    wire::RouteUpdate route() { return dynamic::snapshot(*store).routes(0); }
};
TEST_F(DynamicMembershipTest, GrapherPolicyReportClearsFlagThroughCatalogCommand)
{
    wire::MembershipCommand registration;
    auto* q = registration.mutable_register_();
    auto* p = q->mutable_process();
    p->set_process_id("grapher");
    p->set_instance("grapher-instance");
    p->set_endpoint("grapher:50053");
    p->set_role(wire::PROCESS_ROLE_GRAPHER);
    ASSERT_EQ(call<wire::RegisterResponse>(registration).status().code(), 0);
    auto before = dynamic::snapshot(*store);
    ASSERT_EQ(before.routes_size(), 1);
    before.mutable_routes(0)->set_physical_policy(true);
    ASSERT_TRUE(store->saveMembership(before).ok());
    wire::MembershipCommand command;
    auto* h = command.mutable_heartbeat();
    h->set_process_id("grapher");
    h->set_instance("grapher-instance");
    h->add_stories_without_physical_policy(1);
    auto loaded = store->membershipCommandState(command);
    ASSERT_TRUE(loaded.ok());
    EXPECT_TRUE(dynamic::heartbeatChanges(*loaded, *h));
    ASSERT_EQ(call<wire::HeartbeatResponse>(command).status().code(), 0);
    auto after = dynamic::snapshot(*store);
    EXPECT_FALSE(after.routes(0).physical_policy());
    EXPECT_GT(after.routes(0).revision(), before.routes(0).revision());
    loaded = store->membershipCommandState(command);
    ASSERT_TRUE(loaded.ok());
    EXPECT_FALSE(dynamic::heartbeatChanges(*loaded, *h));
    ASSERT_EQ(call<wire::HeartbeatResponse>(command).status().code(), 0);
    const auto path = (dir.path() / "catalog").string();
    store.reset();
    auto reopened = SqliteMetadataStore::open(path, testing::twoKeeperTopology());
    ASSERT_TRUE(reopened.ok());
    store = std::move(*reopened);
    EXPECT_FALSE(store->membershipRouteUpdate(1)->physical_policy());
}

TEST_F(DynamicMembershipTest, ReplacementRevokesExtensionAndReturnsMonotoneFloors)
{
    ready();
    auto ceiling = extend("keeper-a", "a1", 10000, 500);
    auto replaced = reg("keeper-a", "a2");
    ASSERT_EQ(replaced.status().code(), 0);
    EXPECT_EQ(replaced.ceiling_floor().physical_ns(), ceiling.ceiling().physical_ns());
    EXPECT_EQ(route().route().epoch(), 2u);
    ASSERT_EQ(route().predecessors_size(), 1);
    EXPECT_EQ(route().predecessors(0).instance(), "a1");
    EXPECT_NE(extend("keeper-a", "a1").status().code(), 0);
    EXPECT_NE(reg("keeper-a", "a1").status().code(), 0);
    auto refused = extend("keeper-b", "b1", 0);
    EXPECT_NE(refused.status().code(), 0);
    ASSERT_GT(refused.routes_size(), 0);
    EXPECT_EQ(refused.routes(refused.routes_size() - 1).revision(), route().revision());
    auto renewed = extend("keeper-a", "a2");
    EXPECT_EQ(renewed.status().code(), 0);
    EXPECT_GE(renewed.ceiling().physical_ns(), ceiling.ceiling().physical_ns());
}
TEST_F(DynamicMembershipTest, RecoveredWalKeepsEpochAndNoPredecessor)
{
    ready();
    ASSERT_EQ(reg("keeper-a", "a2", "a1").status().code(), 0);
    EXPECT_EQ(route().route().epoch(), 1u);
    EXPECT_EQ(route().predecessors_size(), 0);
    EXPECT_NE(heartbeat("keeper-a", "a1").status().code(), 0);
}
TEST_F(DynamicMembershipTest, RouteTransitionIsAtomicAndReassignsOnlyRemovedWriters)
{
    ready();
    ASSERT_TRUE(store->createStory("c", "second").ok());
    auto a = store->acquire(1, "first");
    auto b = store->acquire(1, "second");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    const auto survivor = a->assigned_keeper.process_id == "keeper-b" ? *a : *b;
    const auto removed = a->assigned_keeper.process_id == "keeper-a" ? *a : *b;
    ASSERT_EQ(change("keeper-a", 0).status().code(), 0);
    auto state = dynamic::snapshot(*store);
    ASSERT_EQ(state.routes_size(), 2);
    EXPECT_EQ(state.routes(0).revision(), state.routes(1).revision());
    EXPECT_EQ(state.routes(0).route().epoch(), 2u);
    EXPECT_EQ(state.routes(1).route().epoch(), 2u);
    auto surviving = store->acquire(1, survivor.writer_id == a->writer_id ? "first" : "second");
    ASSERT_TRUE(surviving.ok());
    EXPECT_EQ(surviving->assigned_keeper.process_id, survivor.assigned_keeper.process_id);
    auto reassigned = store->acquire(1, removed.writer_id == a->writer_id ? "first" : "second");
    ASSERT_TRUE(reassigned.ok());
    EXPECT_EQ(reassigned->assigned_keeper.process_id, "keeper-b");
    EXPECT_EQ(reassigned->incarnation, removed.incarnation + 1);
    auto backup = (dir.path() / "snapshot").string();
    ASSERT_TRUE(store->backupTo(backup).ok());
    ASSERT_EQ(change("keeper-a", 1).status().code(), 0);
    ASSERT_TRUE(store->installFrom(backup).ok());
    EXPECT_EQ(route().route().epoch(), 2u);
}
TEST_F(DynamicMembershipTest, StoryPhysicalFloorDoesNotFallWhenALowerCeilingKeeperJoins)
{
    ready();
    ASSERT_EQ(reg("keeper-c", "c1").status().code(), 0);
    ASSERT_EQ(extend("keeper-c", "c1", 10000, 100, 10).status().code(), 0);
    ASSERT_EQ(change("keeper-c", 1).status().code(), 0);
    auto floor = route().physical_floor_ns();
    EXPECT_EQ(floor, 5000000100LL);
    ASSERT_EQ(change("keeper-a", 0).status().code(), 0);
    EXPECT_EQ(route().physical_floor_ns(), floor);
    auto snapshot = dynamic::snapshot(*store);
    EXPECT_EQ(snapshot.routes(0).physical_floor_ns(), floor);
    EXPECT_EQ(snapshot.route_history_size(), 0);
}
TEST_F(DynamicMembershipTest, MismatchedPolicyRefusesRegistration)
{
    wire::MembershipCommand q;
    auto* r = q.mutable_register_();
    r->set_policy_version(2);
    auto* p = r->mutable_process();
    p->set_process_id("keeper-a");
    p->set_instance("a1");
    p->set_endpoint("keeper-a:50052");
    p->set_role(wire::PROCESS_ROLE_KEEPER);
    EXPECT_EQ(call<wire::RegisterResponse>(q).status().code(), static_cast<int>(absl::StatusCode::kFailedPrecondition));
}
TEST_F(DynamicMembershipTest, MissingCeilingBlocksEpochChangeAndAbandonmentDeclaresTotalLoss)
{
    ASSERT_EQ(reg("keeper-a", "a1").status().code(), 0);
    ASSERT_EQ(reg("keeper-b", "b1").status().code(), 0);
    EXPECT_NE(change("keeper-a", 0).status().code(), 0);
    EXPECT_EQ(route().route().epoch(), 1u);
    EXPECT_NE(change("keeper-a", 2).status().code(), 0);
    ASSERT_EQ(extend("keeper-b", "b1").status().code(), 0);
    ASSERT_EQ(change("keeper-a", 2).status().code(), 0);
    ASSERT_EQ(route().abandoned_size(), 1);
    EXPECT_EQ(route().abandoned(0).end().physical_ns(), std::numeric_limits<int64_t>::max());
}
TEST_F(DynamicMembershipTest, LastKeeperCannotDrainOrAbandon)
{
    ready();
    ASSERT_EQ(change("keeper-a", 0).status().code(), 0);
    for(int action: {0, 2})
    {
        auto refused = change("keeper-b", action);
        EXPECT_EQ(refused.status().code(), static_cast<int>(absl::StatusCode::kFailedPrecondition));
        EXPECT_NE(refused.status().message().find("1"), std::string::npos);
        EXPECT_EQ(route().route().keepers_size(), 1);
    }
    EXPECT_TRUE(dynamic::wouldEmptyRoute(dynamic::snapshot(*store), "keeper-b"));
}
TEST_F(DynamicMembershipTest, TrimmedHistoryRefusalReturnsCurrentRoutes)
{
    ready();
    ASSERT_EQ(change("keeper-a", 0).status().code(), 0);
    ASSERT_EQ(extend("keeper-a", "a1").status().code(), 0);
    ASSERT_EQ(extend("keeper-b", "b1").status().code(), 0);
    auto state = dynamic::snapshot(*store);
    EXPECT_EQ(state.route_history_size(), 0);
    auto refused = extend("keeper-b", "b1", 0);
    EXPECT_NE(refused.status().code(), 0);
    ASSERT_EQ(refused.routes_size(), 1);
    EXPECT_EQ(refused.routes(0).route().epoch(), 2u);
}
TEST_F(DynamicMembershipTest, OnlyNewOrGrownSettlementProofNeedsProposal)
{
    ready();
    wire::HeartbeatRequest q;
    q.set_process_id("keeper-a");
    q.set_instance("a1");
    EXPECT_FALSE(dynamic::heartbeatChanges(dynamic::snapshot(*store), q));
    auto* f = q.add_story_frontiers();
    f->set_story_id(1);
    auto* proof = f->mutable_settlement();
    proof->set_instance("a1");
    proof->mutable_coverage_start()->set_physical_ns(100);
    proof->mutable_settled_through()->set_physical_ns(200);
    EXPECT_TRUE(dynamic::heartbeatChanges(dynamic::snapshot(*store), q));
    ASSERT_EQ(heartbeat("keeper-a", "a1", *f).status().code(), 0);
    EXPECT_FALSE(dynamic::heartbeatChanges(dynamic::snapshot(*store), q));
    proof->mutable_settled_through()->set_physical_ns(150);
    EXPECT_FALSE(dynamic::heartbeatChanges(dynamic::snapshot(*store), q));
    proof->mutable_settled_through()->set_physical_ns(250);
    EXPECT_TRUE(dynamic::heartbeatChanges(dynamic::snapshot(*store), q));
}
TEST_F(DynamicMembershipTest, ApplyCostFor64KeepersAnd10000Stories)
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    GTEST_SKIP() << "timing bounds hold only in uninstrumented builds";
#endif
    ASSERT_TRUE(store->applyRaft(1,
                                 [&]
                                 {
                                     for(int n = 1; n < 10000; ++n)
                                         if(!store->createStory("c", "s" + std::to_string(n)).ok())
                                             throw std::runtime_error("story setup failed");
                                     return std::string{};
                                 })
                        .ok());
    auto state = dynamic::snapshot(*store);
    for(int n = 0; n < 62; ++n)
    {
        auto* m = state.add_members();
        m->mutable_process()->set_process_id("extra" + std::to_string(n));
        m->mutable_process()->set_endpoint("127.0.0.1:50052");
        m->mutable_process()->set_role(wire::PROCESS_ROLE_KEEPER);
        m->set_joined(true);
    }
    for(auto& m: *state.mutable_members())
    {
        m.mutable_process()->set_instance(m.process().process_id() + "1");
        auto* i = m.add_instances();
        i->set_instance(m.process().instance());
        i->set_granted(true);
        i->mutable_ceiling()->set_physical_ns(5000000100LL);
        i->set_physical_ceiling_ns(5000000100LL);
    }
    for(auto& r: *state.mutable_routes())
        for(int n = 0; n < 62; ++n)
        {
            auto* k = r.mutable_route()->add_keepers();
            k->set_process_id("extra" + std::to_string(n));
            k->set_endpoint("127.0.0.1:50052");
        }
    ASSERT_TRUE(store->saveMembership(state).ok());
    auto measure = [](auto&& apply)
    {
        const auto start = std::chrono::steady_clock::now();
        apply();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    double registerMax = 0, extendMax = 0, registerSum = 0, extendSum = 0;
    double registerApplySum = 0, extendApplySum = 0, registerApplyMax = 0, extendApplyMax = 0;
    for(int n = 0; n < 10; ++n)
    {
        const auto registration = measure([&] { ASSERT_EQ(reg("keeper-a", "keeper-a1").status().code(), 0); });
        registerApplySum += lastApplyMs;
        registerApplyMax = std::max(registerApplyMax, lastApplyMs);
        const auto extension =
                measure([&] { ASSERT_EQ(extend("keeper-a", "keeper-a1", 10000, 100 + n).status().code(), 0); });
        extendApplySum += lastApplyMs;
        extendApplyMax = std::max(extendApplyMax, lastApplyMs);
        registerMax = std::max(registerMax, registration);
        extendMax = std::max(extendMax, extension);
        registerSum += registration;
        extendSum += extension;
    }

    double durableBaselineSum = 0;
    for(int n = 0; n < 10; ++n)
        durableBaselineSum += measure(
                [&] {
                    ASSERT_TRUE(
                            store->applyRaft(store->appliedIndex().value_or(0) + 1, [] { return std::string{}; }).ok());
                });
    EXPECT_LT(registerApplyMax, 20);
    EXPECT_LT(extendApplyMax, 20);
    EXPECT_LT(registerMax, 20);
    EXPECT_LT(extendMax, 20);
    wire::MembershipCommand heartbeatCommand;
    auto* heartbeat = heartbeatCommand.mutable_heartbeat();
    heartbeat->set_process_id("keeper-a");
    heartbeat->set_instance("keeper-a1");
    for(int n = 1; n <= 64; ++n)
    {
        auto* f = heartbeat->add_story_frontiers();
        f->set_story_id(n);
        auto* proof = f->mutable_settlement();
        proof->set_instance("keeper-a1");
        proof->mutable_settled_through()->set_physical_ns(100);
    }
    const auto proofMs =
            measure([&] { ASSERT_EQ(call<wire::HeartbeatResponse>(heartbeatCommand).status().code(), 0); });
    EXPECT_LT(proofMs, 20);
    auto after = dynamic::snapshot(*store);
    ASSERT_EQ(after.members_size(), 64);
    ASSERT_EQ(after.routes_size(), 10000);
    bool found = false;
    for(const auto& m: after.members())
        if(m.process().process_id() == "keeper-a")
            for(const auto& i: m.instances())
                if(i.instance() == "keeper-a1")
                {
                    EXPECT_EQ(i.proofs_size(), 64);
                    found = true;
                }
    EXPECT_TRUE(found);
    const auto beforeRestriction = after;
    // Restrict one Keeper to three stories outside the measured transition.
    for(int n = 3; n < after.routes_size(); ++n)
    {
        auto* keepers = after.mutable_routes(n)->mutable_route()->mutable_keepers();
        for(int k = keepers->size() - 1; k >= 0; --k)
            if(keepers->Get(k).process_id() == "extra61")
                keepers->DeleteSubrange(k, 1);
    }
    after.clear_route_history();
    ASSERT_TRUE(store->applyRaft(store->appliedIndex().value_or(0) + 1,
                                 [&]
                                 {
                                     auto status = store->saveMembershipChanges(beforeRestriction, after);
                                     if(!status.ok())
                                         throw std::runtime_error(status.ToString());
                                     return std::string{};
                                 })
                        .ok());
    const auto transitionMs = measure([&] { ASSERT_EQ(change("extra61", 0).status().code(), 0); });
    EXPECT_LT(transitionMs, 60);
    auto routes = store->membershipRouteChanges(0);
    ASSERT_TRUE(routes.ok());
    for(int n = 0; n < 10000; ++n)
    {
        auto r = store->membershipRouteUpdate(n + 1);
        ASSERT_TRUE(r.ok());
        EXPECT_EQ(r->route().epoch(), n < 3 ? 2u : 1u);
    }
    std::cout << "64 Keepers / 10000 stories: state apply Register mean/max " << registerApplySum / 10 << "/"
              << registerApplyMax << " ms; state apply ExtendCeiling mean/max " << extendApplySum / 10 << "/"
              << extendApplyMax << " ms; durable no-op mean " << durableBaselineSum / 10
              << " ms; durable Register mean/max " << registerSum / 10 << "/" << registerMax
              << " ms; ExtendCeiling mean/max " << extendSum / 10 << "/" << extendMax << " ms; heartbeat 64 proofs "
              << proofMs << " ms; transition 3 stories " << transitionMs << " ms" << std::endl;
}
TEST_F(DynamicMembershipTest, MigratesLegacyMembershipAndPreservesSnapshotInstall)
{
    ready();
    ASSERT_EQ(change("keeper-a", 0).status().code(), 0);
    const auto before = dynamic::snapshot(*store);
    const auto path = (dir.path() / "catalog").string();
    store.reset();
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
                           "BEGIN; DELETE FROM membership_meta; INSERT INTO membership_state(value) VALUES(''); UPDATE "
                           "schema_version SET version=1;",
                           nullptr,
                           nullptr,
                           nullptr),
              SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "UPDATE membership_state SET value=?1", -1, &statement, nullptr), SQLITE_OK);
    const auto legacy = before.SerializeAsString();
    ASSERT_EQ(sqlite3_bind_blob(statement, 1, legacy.data(), static_cast<int>(legacy.size()), SQLITE_TRANSIENT),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
    // Model a version-one database whose only membership storage was the blob.
    ASSERT_EQ(sqlite3_exec(db,
                           "DELETE FROM membership_members; DELETE FROM membership_instances; DELETE FROM "
                           "membership_proofs; DELETE FROM membership_routes; DELETE FROM membership_route_refs; "
                           "DELETE FROM membership_history; COMMIT;",
                           nullptr,
                           nullptr,
                           nullptr),
              SQLITE_OK);
    sqlite3_close(db);
    auto reopened = SqliteMetadataStore::open(path, testing::twoKeeperTopology());
    ASSERT_TRUE(reopened.ok()) << reopened.status();
    store = std::move(*reopened);
    EXPECT_EQ(dynamic::snapshot(*store).SerializeAsString(), before.SerializeAsString());
    auto backup = (dir.path() / "membership-snapshot").string();
    ASSERT_TRUE(store->backupTo(backup).ok());
    ASSERT_EQ(change("keeper-a", 1).status().code(), 0);
    ASSERT_TRUE(store->installFrom(backup).ok());
    EXPECT_EQ(dynamic::snapshot(*store).SerializeAsString(), before.SerializeAsString());
}

TEST_F(DynamicMembershipTest, UpgradesNormalizedInstanceGrantIndex)
{
    ready();
    const auto before = dynamic::snapshot(*store);
    const auto path = (dir.path() / "catalog").string();
    store.reset();
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(
            sqlite3_exec(db,
                         "BEGIN; DROP INDEX membership_granted; ALTER TABLE membership_instances RENAME TO "
                         "old_instances; CREATE TABLE membership_instances(process_id TEXT NOT NULL,instance TEXT NOT "
                         "NULL,value BLOB NOT NULL,PRIMARY KEY(process_id,instance)); INSERT INTO membership_instances "
                         "SELECT process_id,instance,value FROM old_instances; DROP TABLE old_instances; COMMIT;",
                         nullptr,
                         nullptr,
                         nullptr),
            SQLITE_OK);
    sqlite3_close(db);
    auto reopened = SqliteMetadataStore::open(path, testing::twoKeeperTopology());
    ASSERT_TRUE(reopened.ok()) << reopened.status();
    store = std::move(*reopened);
    EXPECT_EQ(dynamic::snapshot(*store).SerializeAsString(), before.SerializeAsString());
    ASSERT_EQ(change("keeper-a", 0).status().code(), 0);
    EXPECT_EQ(route().route().epoch(), 2u);
}

} // namespace chronolog::visor
