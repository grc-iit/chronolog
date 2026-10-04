#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <unistd.h>
#include "player/adapter/Convert.h"
#include "player/replay/HotReplay.h"
#include "player/replay/KeeperHotSource.h"
#include "keeper/adapter/ArchiveService.h"
#include "keeper/membership/ConfigMembership.h"
#include "common/clock/FakeClock.h"
#include "common/worker/WorkerPool.h"
#include "keeper/tests/ram_harness.h"
#include "keeper/tests/wal_harness.h"

namespace chronolog::player
{
namespace
{
namespace wire = internal::v1;
Event event(int64_t time, uint64_t writer = 2)
{
    Event e;
    e.id = {1, writer, 1, static_cast<uint64_t>(time)};
    e.hlc = {time, 0};
    e.physical = {time, 0, ClockStatus::Synced};
    e.durability = Durability::Durable;
    return e;
}
class Routes final: public RouteSource
{
public:
    RouteState state;
    bool policy{};
    absl::StatusOr<Route> route(StoryId) const override { return state.route; }
    absl::StatusOr<RouteState> routeState(StoryId) const override { return state; }
    bool physicalPolicy(StoryId) const override { return policy; }
    int64_t skewLimitNs() const override { return 10; }
};
class KeeperDriver final: public wire::Archive::Service
{
public:
    Epoch epoch{2};
    std::string instance{"current"};
    Hlc seal{1000, 0};
    int64_t physical{1000};
    bool failed{}, truncated{}, lie{};
    std::vector<Event> events;
    std::mutex mutex;
    std::vector<wire::FetchHotRequest> requests;
    grpc::Status FetchHot(grpc::ServerContext*,
                          const wire::FetchHotRequest* q,
                          grpc::ServerWriter<wire::FetchHotResponse>* writer) override
    {
        std::lock_guard lock(mutex);
        requests.push_back(*q);
        if(failed)
            return {grpc::StatusCode::UNAVAILABLE, "failed source"};
        if(!lie && ((!q->expect_instance().empty() && q->expect_instance() != instance) || q->expect_epoch() != epoch))
            return {grpc::StatusCode::FAILED_PRECONDITION, "different owner"};
        wire::FetchHotResponse batch;
        for(const auto& e: events)
        {
            bool match = q->has_hlc() ? e.hlc >= convert::fromProto(q->hlc().start()) &&
                                                e.hlc < convert::fromProto(q->hlc().end())
                                      : true;
            if(match && q->has_physical_filter())
            {
                const auto& r = q->physical_filter();
                const bool bounded = e.physical.status == ClockStatus::Synced && e.physical.uncertainty_ns &&
                                     *e.physical.uncertainty_ns <= PhysicalPolicy{}.uncertainty_cap_ns;
                const __int128_t p = e.physical.physical_ns;
                const __int128_t u = bounded ? *e.physical.uncertainty_ns : 0;
                match = bounded ? p - u < r.end_ns() && p + u >= r.start_ns() : p >= r.start_ns() && p < r.end_ns();
            }
            if(match)
                *batch.mutable_batch()->add_events() = convert::toProto(e);
        }
        if(batch.has_batch() && !writer->Write(batch))
            return grpc::Status::CANCELLED;
        wire::FetchHotResponse end;
        auto* trailer = end.mutable_trailer();
        trailer->set_epoch(epoch);
        trailer->set_instance(instance);
        *trailer->mutable_sealed_frontier() = convert::toProto(seal);
        trailer->set_physical_frontier_ns(physical);
        trailer->set_truncated(truncated);
        writer->Write(end);
        return grpc::Status::OK;
    }
};
class ReplayContract: public ::testing::Test
{
protected:
    KeeperDriver current, old;
    std::shared_ptr<FakeClock> real_clock;
    std::shared_ptr<keeper::ConfigMembership> real_membership;
    std::unique_ptr<RamJournal> real_journal;
    std::unique_ptr<WorkerPool> real_pool;
    std::unique_ptr<keeper::ArchiveService> real_archive;
    void useRealPredecessor(std::string instance = "old-instance")
    {
        old_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        old_server.reset();
        real_clock = std::make_shared<FakeClock>(140, 0);
        real_clock->setStatus(ClockStatus::Synced);
        real_membership = std::make_shared<keeper::ConfigMembership>(
                std::vector<keeper::StaticRoute>{{1, {1, {{"old", "old:1"}}, "", ""}}});
        RamJournalConfig config;
        config.process_id = "old";
        config.instance = instance;
        real_journal = std::make_unique<RamJournal>(real_clock, real_membership, config);
        real_journal->enableDynamic(instance);
        real_journal->extendCeiling({1000, 0}, 300);
        RouteState initial;
        initial.route = *real_membership->route(1);
        real_journal->applyRoute(1, initial, false, 1, [] {});
        if(!real_journal->registerWriter(1, 2, 1).ok())
            throw std::runtime_error("writer registration failed");
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 1;
        item.sequence = 1;
        item.physical = {140, 0, ClockStatus::Synced};
        auto appended = real_journal->append({1, 1, {item}}, Durability::Accepted);
        if(!appended.ok() || !appended->front().status.ok())
            throw std::runtime_error("real Keeper append failed");
        RouteState retired = routes->state;
        retired.predecessors.front().instance = instance;
        real_journal->applyRoute(1, retired, false, 2, [&] { real_membership->setRouteState(1, retired); });
        real_pool = std::make_unique<WorkerPool>(2, 16);
        real_archive = std::make_unique<keeper::ArchiveService>(*real_journal, *real_membership, *real_pool);
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(real_archive.get());
        old_server = builder.BuildAndStart();
        if(!old_server)
            throw std::runtime_error("real Keeper server failed");
        routes->state.predecessors.front().keeper.endpoint = "127.0.0.1:" + std::to_string(port);
    }
    std::unique_ptr<grpc::Server> current_server, old_server;
    std::shared_ptr<Routes> routes = std::make_shared<Routes>();
    std::shared_ptr<KeeperHotSource> source;
    std::unique_ptr<FileTierStore> archive_writer;
    HotReplayOptions options;
    std::filesystem::path root;
    std::vector<Event> returned;
    Completion completion;
    std::string start(KeeperDriver& driver, std::unique_ptr<grpc::Server>& server)
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&driver);
        server = builder.BuildAndStart();
        if(!server)
            throw std::runtime_error("server failed");
        return "127.0.0.1:" + std::to_string(port);
    }
    void SetUp() override
    {
        auto endpoint = start(current, current_server);
        auto predecessor_endpoint = start(old, old_server);
        routes->state.route = {2, {{"current", endpoint}}, "", ""};
        routes->state.ordering_cut = {900, 0};
        routes->state.predecessors = {{{"old", predecessor_endpoint}, "old-instance", 1, {200, 0}, 300}};
        old.epoch = 1;
        old.instance = "old-instance";
        old.seal = {200, 0};
        old.events = {event(140)};
        current.events = {event(240, 4)};
        source = std::make_shared<KeeperHotSource>(
                routes,
                nullptr,
                [](const KeeperRef& k) { return k.endpoint; },
                KeeperHotSourceOptions{std::chrono::milliseconds(500), 0, 100});
        char pattern[] = "/tmp/chronolog-replay-m8-XXXXXX";
        root = ::mkdtemp(pattern);
        auto opened = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
        ASSERT_TRUE(opened.ok()) << opened.status();
        archive_writer = *std::move(opened);
        auto reader = FileTierStore::OpenReadOnly(root, std::chrono::hours(1));
        ASSERT_TRUE(reader.ok()) << reader.status();
        options.archive = std::shared_ptr<FileTierStore>(*std::move(reader));
        options.batch_size = 2;
    }
    void TearDown() override
    {
        source.reset();
        current_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        old_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        options.archive.reset();
        archive_writer.reset();
        std::filesystem::remove_all(root);
    }
    void read(Range range = {Range::Axis::Hlc, {100, 0}, {300, 0}})
    {
        HotReplay replay(source, options);
        auto stream = replay.read(1, range);
        ASSERT_TRUE(stream.ok()) << stream.status();
        returned.clear();
        int completions = 0;
        for(int n = 0; n < 100; ++n)
        {
            auto b = (*stream)->next();
            ASSERT_TRUE(b.ok()) << b.status();
            if(!*b)
            {
                EXPECT_EQ(completions, 1);
                return;
            }
            EXPECT_EQ(completions, 0);
            returned.insert(returned.end(), (**b).events.begin(), (**b).events.end());
            if((**b).completion)
            {
                completion = *(**b).completion;
                ++completions;
            }
        }
        FAIL() << "unbounded stream";
    }
};
TEST_F(ReplayContract, UndrainedPredecessorIsASource)
{
    useRealPredecessor();
    read();
    EXPECT_TRUE(completion.complete);
    ASSERT_EQ(returned.size(), 2);
    EXPECT_EQ(returned[0].hlc, (Hlc{140, 0}));
    EXPECT_EQ(real_journal->keeperFrontier(1).value(), (Hlc{200, 0}));
}
TEST_F(ReplayContract, DeadPredecessorIsSourceFailed)
{
    useRealPredecessor();
    old_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
    read();
    EXPECT_FALSE(completion.complete);
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, ReplacementInstanceCannotAnswerForItsPredecessor)
{
    useRealPredecessor("replacement");
    read();
    EXPECT_FALSE(completion.complete);
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, TrailerEpochMismatchIsSourceFailed)
{
    current.epoch = 3;
    current.lie = true;
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, DrainedPredecessorHistoryComesFromTheArchive)
{
    useRealPredecessor();
    ASSERT_TRUE(archive_writer->publish({"old", 1, {100, 0}, {200, 0}, old.events, false}).ok());
    routes->state.predecessors.clear();
    routes->state.archived_below = {200, 0};
    read();
    EXPECT_TRUE(completion.complete);
    ASSERT_EQ(returned.size(), 2);
    EXPECT_EQ(returned[0].hlc, (Hlc{140, 0}));
    EXPECT_TRUE(old.requests.empty());
}
TEST_F(ReplayContract, TruncatedReadFrontierIsACompletePrefix)
{
    old.events = {event(120), event(180)};
    old.truncated = true;
    old.seal = {160, 0};
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{160, 0}));
    ASSERT_EQ(returned.size(), 1);
    EXPECT_LT(returned[0].hlc, completion.frontier);
    std::vector<Event> first = returned;
    old.truncated = false;
    old.seal = {200, 0};
    read({Range::Axis::Hlc, completion.frontier, {300, 0}});
    EXPECT_TRUE(completion.complete);
    first.insert(first.end(), returned.begin(), returned.end());
    ASSERT_EQ(first.size(), 3);
    EXPECT_EQ(first[0].hlc, (Hlc{120, 0}));
    EXPECT_EQ(first[1].hlc, (Hlc{180, 0}));
    EXPECT_EQ(first[2].hlc, (Hlc{240, 0}));
}
TEST_F(ReplayContract, OwnCutControlsCoverageAndSourceSelection)
{
    old.seal = {190, 0};
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::LaggingWriters);
    old.requests.clear();
    read({Range::Axis::Hlc, {200, 0}, {300, 0}});
    EXPECT_TRUE(completion.complete);
    EXPECT_TRUE(old.requests.empty());
}
TEST_F(ReplayContract, AbandonedRangesStaySourceFailed)
{
    routes->state.predecessors.clear();
    routes->state.abandoned = {{Range::Axis::Hlc, {150, 0}, {200, 0}}};
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
    read({Range::Axis::Hlc, {200, 0}, {300, 0}});
    EXPECT_TRUE(completion.complete);
    read({Range::Axis::Physical, {400, 0}, {500, 0}});
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, PhysicalPredecessorSpanIncludesPhysicalCeilingAndSkew)
{
    routes->policy = true;
    old.events.clear();
    read({Range::Axis::Physical, {305, 0}, {320, 0}});
    ASSERT_EQ(old.requests.size(), 1);
    EXPECT_TRUE(old.requests[0].has_hlc());
    ASSERT_TRUE(old.requests[0].has_physical_filter());
    EXPECT_EQ(old.requests[0].physical_filter().start_ns(), 305);
    EXPECT_EQ(old.requests[0].physical_filter().end_ns(), 320);
    old.requests.clear();
    read({Range::Axis::Physical, {310, 0}, {320, 0}});
    EXPECT_TRUE(old.requests.empty());
    routes->state.predecessors[0].own_physical_ceiling_ns = std::numeric_limits<int64_t>::max() - 3;
    read({Range::Axis::Physical,
          {std::numeric_limits<int64_t>::max() - 2, 0},
          {std::numeric_limits<int64_t>::max(), 0}});
    EXPECT_EQ(old.requests.size(), 1);
}
TEST_F(ReplayContract, PredecessorPhysicalFrontierMustCoverEnd)
{
    routes->policy = true;
    Chunk chunk{"empty", 1, {100, 0}, {1000, 0}, {}, true};
    chunk.physical_policy = true;
    ASSERT_TRUE(archive_writer->publish(chunk).ok());
    old.events.clear();
    old.physical = 309;
    read({Range::Axis::Physical, {305, 0}, {320, 0}});
    EXPECT_EQ(completion.reason, IncompleteReason::LaggingWriters);
    old.physical = 320;
    read({Range::Axis::Physical, {305, 0}, {320, 0}});
    EXPECT_TRUE(completion.complete);
}
TEST_F(ReplayContract, PhysicalReadWithoutEvictionsDoesNotRequireAnArchive)
{
    routes->policy = true;
    options.archive.reset();
    read({Range::Axis::Physical, {100, 0}, {200, 0}});
    EXPECT_TRUE(completion.complete);
    ASSERT_EQ(returned.size(), 1);
    EXPECT_EQ(returned[0].hlc, (Hlc{140, 0}));
}
TEST_F(ReplayContract, ArchiveWithoutPolicyMarkerMakesPhysicalReadIncomplete)
{
    routes->policy = true;
    old.events.clear();
    current.events.clear();
    routes->state.archived_below = {200, 0};
    ASSERT_TRUE(archive_writer->publish({"legacy", 1, {100, 0}, {200, 0}, {event(140)}, false}).ok());
    read({Range::Axis::Physical, {135, 0}, {145, 0}});
    EXPECT_FALSE(completion.complete);
    EXPECT_EQ(completion.reason, IncompleteReason::PhysicalAxisUnbounded);
    ASSERT_EQ(returned.size(), 1);
    EXPECT_EQ(returned[0].hlc, (Hlc{140, 0}));
}

TEST_F(ReplayContract, PhysicalLimitCountsMatchesAndKeepsTheRequestedEnd)
{
    routes->policy = true;
    options.read_max_events = 1;
    routes->state.archived_below = {200, 0};
    Chunk chunk{"physical", 1, {100, 0}, {200, 0}, {event(120), event(140)}, false};
    chunk.physical_policy = true;
    ASSERT_TRUE(archive_writer->publish(chunk).ok());
    old.events.clear();
    current.events.clear();
    read({Range::Axis::Physical, {135, 0}, {145, 0}});
    EXPECT_TRUE(completion.complete);
    ASSERT_EQ(returned.size(), 1);
    EXPECT_EQ(returned[0].hlc, (Hlc{140, 0}));
    read({Range::Axis::Physical, {100, 0}, {200, 0}});
    EXPECT_FALSE(completion.complete);
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_LE(returned.size(), 1);
}
TEST_F(ReplayContract, TruncatedPrefixAlsoRespectsCurrentSealAndArchiveRecordStart)
{
    old.events = {event(120), event(180)};
    old.truncated = true;
    old.seal = {190, 0};
    current.seal = {175, 0};
    routes->state.archived_below = {200, 0};
    ASSERT_TRUE(archive_writer->publish({"first", 1, {100, 0}, {150, 0}, {event(120)}, false}).ok());
    ASSERT_TRUE(archive_writer->publish({"second", 1, {150, 0}, {200, 0}, {event(160)}, false}).ok());
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{150, 0}));
    ASSERT_EQ(returned.size(), 1);
    EXPECT_EQ(returned[0].hlc, (Hlc{120, 0}));
}
TEST_F(ReplayContract, LostArchiveWindowCannotLieBelowTruncatedPrefix)
{
    old.events = {event(120), event(180)};
    old.truncated = true;
    routes->state.archived_below = {200, 0};
    ASSERT_TRUE(archive_writer->publish({"first", 1, {100, 0}, {150, 0}, {event(120)}, false}).ok());
    ASSERT_TRUE(archive_writer->publish({"lost", 1, {150, 0}, {200, 0}, {event(160)}, false}).ok());
    auto records = archive_writer->manifest(1);
    ASSERT_TRUE(records.ok());
    for(const auto& r: *records)
        if(r.start == Hlc{150, 0})
            std::filesystem::remove(root / r.file);
    archive_writer.reset();
    auto recovered = FileTierStore::Open(root, "writer", {{1, {100, 0}}});
    ASSERT_TRUE(recovered.ok());
    archive_writer = *std::move(recovered);
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{150, 0}));
    ASSERT_EQ(returned.size(), 1);
    EXPECT_EQ(returned[0].hlc, (Hlc{120, 0}));
}
TEST_F(ReplayContract, PredecessorOwnedEpochMismatchIsSourceFailed)
{
    old.epoch = 2;
    old.lie = true;
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::SourceFailed);
}
TEST_F(ReplayContract, TruncatedFrontierDoesNotPassASealBelowTheQueryStart)
{
    current.seal = {90, 0};
    old.truncated = true;
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{90, 0}));
    EXPECT_TRUE(returned.empty());
}
TEST_F(ReplayContract, TruncatedEmptyPredecessorHasNoCompletePrefix)
{
    old.events.clear();
    old.truncated = true;
    read();
    EXPECT_EQ(completion.reason, IncompleteReason::Truncated);
    EXPECT_EQ(completion.frontier, (Hlc{100, 0}));
    EXPECT_TRUE(returned.empty());
}
// A real Keeper behind its own server, so the Tail rule meets the seal the Keeper really computes. The seal ticks F,
// scans writer 2 (still empty), and writers 2 and 4 then assign above F before the scan reaches writer 4, whose event it
// returns: no DURABLE event and no second Keeper is needed for an event at or above F to arrive before one below it.
class RealKeeperTail: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(rig.journal->registerWriter(1, 4, 3).ok());
        start(*rig.journal, *rig.membership);
    }
    void start(RamJournal& journal, Membership& membership)
    {
        pool = std::make_unique<WorkerPool>(2, 16);
        archive = std::make_unique<keeper::ArchiveService>(journal, membership, *pool);
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(archive.get());
        server = builder.BuildAndStart();
        ASSERT_NE(server, nullptr);
        routes->state.route = {7, {{"self", "127.0.0.1:" + std::to_string(port)}}, "", ""};
        source = std::make_shared<KeeperHotSource>(
                routes,
                nullptr,
                [](const KeeperRef& k) { return k.endpoint; },
                KeeperHotSourceOptions{std::chrono::milliseconds(2000), 0, 100});
    }
    void TearDown() override
    {
        rig.journal->onScanned({});
        source.reset();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
    }
    void armLostEvent()
    {
        rig.journal->onScanned(
                [this]
                {
                    if(fired.exchange(true))
                        return;
                    for(uint64_t writer: {2, 4})
                    {
                        AppendItem item;
                        item.writer_id = writer;
                        item.incarnation = 3;
                        item.sequence = 1;
                        auto appended = rig.journal->append({1, 7, {item}}, Durability::Accepted);
                        EXPECT_TRUE(appended.ok());
                        if(appended.ok())
                        {
                            std::lock_guard lock(assigned_mu);
                            assigned.push_back(appended->front().hlc);
                        }
                    }
                });
    }
    test::RamRig rig;
    std::unique_ptr<test::WalRig> wal;
    std::unique_ptr<WorkerPool> pool;
    std::unique_ptr<keeper::ArchiveService> archive;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<Routes> routes = std::make_shared<Routes>();
    std::shared_ptr<KeeperHotSource> source;
    std::vector<Hlc> assignedByTheHook()
    {
        std::lock_guard lock(assigned_mu);
        return assigned;
    }
    std::atomic<bool> fired{false};
    std::mutex assigned_mu;
    std::vector<Hlc> assigned;
};
TEST_F(RealKeeperTail, TailDeliversAPendingDurableEventBelowAVisibleOne)
{
    server->Shutdown();
    server.reset();
    archive.reset();
    pool.reset();
    wal = std::make_unique<test::WalRig>();
    ASSERT_TRUE(wal->journal->registerWriter(1, 4, 3).ok());
    start(*wal->journal, *wal->membership);
    std::future<absl::StatusOr<std::vector<AppendResult>>> durable;
    Hlc pending;
    Hlc visible;
    std::promise<void> scheduled;
    auto schedule = scheduled.get_future();
    // FetchHot has persisted its physical frontier when sealedRead ticks. Hold the event sync from that tick onward.
    wal->clock->ticked = [&](Hlc)
    {
        if(fired.exchange(true))
            return;
        wal->control->block();
        AppendItem item;
        item.writer_id = 2;
        item.incarnation = 3;
        item.sequence = 1;
        durable = std::async(std::launch::async,
                             [&, item] { return wal->journal->append({1, 7, {item}}, Durability::Durable); });
        pending = wal->control->waitPending();
        item.writer_id = 4;
        auto accepted = wal->journal->append({1, 7, {item}}, Durability::Accepted);
        EXPECT_TRUE(accepted.ok());
        if(accepted.ok())
            visible = accepted->front().hlc;
        scheduled.set_value();
    };
    HotReplayOptions options;
    options.tail_poll = std::chrono::milliseconds(1);
    HotReplay replay(source, options);
    Event position;
    position.id.story_id = 1;
    auto stream = replay.tail(1, position);
    wal->control->release();
    ASSERT_TRUE(stream.ok()) << stream.status();
    ASSERT_EQ(schedule.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    schedule.get();
    ASSERT_TRUE(durable.valid());
    auto committed = durable.get();
    ASSERT_TRUE(committed.ok());
    ASSERT_EQ(committed->size(), 1u);
    ASSERT_TRUE(committed->front().status.ok());
    ASSERT_LT(pending, visible);
    auto pulled = std::async(std::launch::async,
                             [&]
                             {
                                 std::vector<Event> events;
                                 for(unsigned i = 0; i < 4 && events.size() < 2; ++i)
                                 {
                                     auto batch = (*stream)->next();
                                     if(!batch.ok() || !*batch || (**batch).completion)
                                         break;
                                     events.insert(events.end(), (**batch).events.begin(), (**batch).events.end());
                                 }
                                 return events;
                             });
    if(pulled.wait_for(std::chrono::seconds(8)) != std::future_status::ready)
        (*stream)->cancel();
    auto events = pulled.get();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].hlc, pending);
    EXPECT_EQ(events[0].durability, Durability::Durable);
    EXPECT_EQ(events[1].hlc, visible);
}

TEST_F(RealKeeperTail, FetchHotReturnsEventsAtOrAboveItsSeal)
{
    armLostEvent();
    auto fetched = source->fetchTail(1, Hlc{}, {});
    ASSERT_TRUE(fetched.ok()) << fetched.status();
    const auto assigned = assignedByTheHook();
    ASSERT_EQ(assigned.size(), 2u);
    ASSERT_EQ(fetched->keepers.size(), 1u);
    ASSERT_EQ(fetched->keepers[0].events.size(), 1u);
    EXPECT_EQ(fetched->keepers[0].events[0].id.writer_id, 4u);
    EXPECT_LT(fetched->keepers[0].frontier.sealed, assigned[0]);
    EXPECT_LT(assigned[0], fetched->keepers[0].events[0].hlc);
}
TEST_F(RealKeeperTail, TailDeliversAnEventAssignedWhileTheSealScansAnotherWriter)
{
    armLostEvent();
    HotReplayOptions options;
    options.tail_poll = std::chrono::milliseconds(5);
    HotReplay replay(source, options);
    Event start;
    start.id.story_id = 1;
    auto stream = replay.tail(1, start);
    ASSERT_TRUE(stream.ok()) << stream.status();
    auto pulled = std::async(std::launch::async,
                             [&]
                             {
                                 std::vector<Event> got;
                                 while(got.size() < 2)
                                 {
                                     auto batch = (*stream)->next();
                                     if(!batch.ok() || !*batch)
                                         break;
                                     got.insert(got.end(), (**batch).events.begin(), (**batch).events.end());
                                 }
                                 return got;
                             });
    if(pulled.wait_for(std::chrono::seconds(8)) != std::future_status::ready)
        (*stream)->cancel();
    auto got = pulled.get();
    const auto assigned = assignedByTheHook();
    ASSERT_EQ(got.size(), 2u);
    ASSERT_EQ(assigned.size(), 2u);
    EXPECT_EQ(got[0].id.writer_id, 2u);
    EXPECT_EQ(got[0].hlc, assigned[0]);
    EXPECT_EQ(got[1].id.writer_id, 4u);
    EXPECT_EQ(got[1].hlc, assigned[1]);
}
} // namespace
} // namespace chronolog::player
