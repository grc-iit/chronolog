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
        auto applied =
                store->applyRaft(store->appliedIndex().value_or(0) + 1, [&] { return dynamic::apply(*store, q); });
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
        m->mutable_process()->set_role(wire::PROCESS_ROLE_KEEPER);
    }
    for(auto& r: *state.mutable_routes())
        for(int n = 0; n < 62; ++n)
        {
            auto* k = r.mutable_route()->add_keepers();
            k->set_process_id("extra" + std::to_string(n));
            k->set_endpoint("127.0.0.1:50052");
        }
    ASSERT_TRUE(store->saveMembership(state).ok());
    auto start = std::chrono::steady_clock::now();
    ASSERT_EQ(reg("keeper-a", "a1").status().code(), 0);
    std::cout << "64 Keeper 10000 story apply ms: "
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
              << std::endl;
}
} // namespace chronolog::visor
