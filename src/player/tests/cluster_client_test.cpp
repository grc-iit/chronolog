#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <condition_variable>
#include <future>
#include "common/rpc/Channel.h"
#include "player/adapter/ClusterClient.h"
#include "player/replay/KeeperHotSource.h"
#include "player/replay/HotReplay.h"

namespace chronolog::player
{
namespace
{
using namespace std::chrono_literals;
namespace wire = internal::v1;
wire::RouteUpdate update(StoryId story = 42, Epoch epoch = 1, uint64_t revision = 1)
{
    wire::RouteUpdate r;
    r.set_story_id(story);
    r.set_revision(revision);
    r.mutable_route()->set_epoch(epoch);
    auto* k = r.mutable_route()->add_keepers();
    k->set_process_id("keeper-1");
    k->set_endpoint("keeper:50052");
    r.set_physical_policy(true);
    r.mutable_ordering_cut()->set_physical_ns(900);
    r.mutable_archived_below()->set_physical_ns(revision);
    auto* p = r.add_predecessors();
    p->mutable_keeper()->set_process_id("old");
    p->mutable_keeper()->set_endpoint("old:50052");
    p->set_instance("old-instance");
    p->set_epoch(1);
    p->mutable_own_cut()->set_physical_ns(200);
    p->set_own_physical_ceiling_ns(300);
    auto* lost = r.add_abandoned();
    lost->mutable_start()->set_physical_ns(120);
    lost->mutable_end()->set_physical_ns(150);
    return r;
}
class CatalogSnapshot final: public wire::Cluster::Service
{
public:
    std::mutex mu;
    std::condition_variable cv;
    std::vector<wire::RouteUpdate> snapshot{update()}, messages;
    std::atomic<int> calls{}, watches{}, heartbeats{};
    bool overflow{}, unknown{}, unavailable{}, obsolete{}, hold_marker{}, hold_entries{};
    uint64_t marker_revision{};
    std::atomic<int> markers{}, total_lookups{};
    grpc::Status Register(grpc::ServerContext*, const wire::RegisterRequest*, wire::RegisterResponse* r) override
    {
        ++calls;
        std::lock_guard lock(mu);
        for(const auto& item: snapshot) *r->add_routes() = item;
        PhysicalPolicy expected;
        auto* p = r->mutable_policy();
        p->set_version(expected.version);
        p->set_skew_limit_ns(expected.skew_limit_ns);
        p->set_acceptance_window_ns(expected.acceptance_window_ns);
        p->set_hlc_lead_ns(expected.hlc_lead_ns);
        p->set_uncertainty_cap_ns(expected.uncertainty_cap_ns);
        unknown = false;
        cv.notify_all();
        return grpc::Status::OK;
    }
    grpc::Status Heartbeat(grpc::ServerContext*, const wire::HeartbeatRequest*, wire::HeartbeatResponse* r) override
    {
        ++heartbeats;
        std::lock_guard lock(mu);
        cv.notify_all();
        if(unavailable)
            return {grpc::StatusCode::UNAVAILABLE, "unavailable"};
        if(unknown || obsolete)
            r->mutable_status()->set_code(static_cast<int>(absl::StatusCode::kFailedPrecondition));
        cv.notify_all();
        return grpc::Status::OK;
    }
    grpc::Status
    ListMembers(grpc::ServerContext*, const wire::ListMembersRequest*, wire::ListMembersResponse* r) override
    {
        std::lock_guard lock(mu);
        if(!unknown)
        {
            auto* process = r->add_members()->mutable_process();
            process->set_process_id("player");
            process->set_instance(obsolete ? "newer" : "instance");
        }
        return grpc::Status::OK;
    }
    grpc::Status WatchRoutes(grpc::ServerContext* context,
                             const wire::WatchRoutesRequest*,
                             grpc::ServerWriter<wire::WatchRoutesResponse>* writer) override
    {
        std::unique_lock lock(mu);
        auto initial = snapshot;
        auto cursor = messages.size();
        uint64_t revision = marker_revision;
        if(!revision)
        {
            for(const auto& item: initial) revision = std::max(revision, item.revision());
            for(const auto& item: messages) revision = std::max(revision, item.revision());
        }
        for(auto& item: initial) item.set_revision(revision);
        ++watches;
        cv.notify_all();
        auto send = [&](const wire::RouteUpdate& item)
        {
            wire::WatchRoutesResponse response;
            response.ParseFromString(item.SerializeAsString());
            return writer->Write(response);
        };
        while(hold_entries && !context->IsCancelled()) cv.wait_for(lock, 10ms);
        lock.unlock();
        for(const auto& item: initial)
            if(!send(item))
                return grpc::Status::OK;
        lock.lock();
        while(hold_marker && !overflow && !context->IsCancelled()) cv.wait_for(lock, 10ms);
        if(!overflow && !context->IsCancelled())
        {
            wire::WatchRoutesResponse marker;
            marker.set_revision(revision);
            marker.set_snapshot_end(true);
            lock.unlock();
            if(!writer->Write(marker))
                return grpc::Status::OK;
            lock.lock();
            ++markers;
            cv.notify_all();
        }
        while(!context->IsCancelled())
        {
            if(overflow)
            {
                overflow = false;
                return {grpc::StatusCode::RESOURCE_EXHAUSTED, "resubscribe"};
            }
            if(cursor < messages.size())
            {
                const auto item = messages[cursor++];
                lock.unlock();
                const auto sent = send(item);
                lock.lock();
                if(!sent)
                    break;
            }
            else
                cv.wait_for(lock, 10ms);
        }
        return grpc::Status::OK;
    }
    void publish(wire::RouteUpdate item)
    {
        std::lock_guard lock(mu);
        std::erase_if(snapshot, [&](const auto& old) { return old.story_id() == item.story_id(); });
        if(!item.tombstoned())
            snapshot.push_back(item);
        messages.push_back(std::move(item));
        cv.notify_all();
    }
    bool subscribed(int count = 1)
    {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, 3s, [&] { return watches >= count; });
    }
};
class DestroyedArchive final: public wire::Archive::Service
{
public:
    std::atomic<int> calls{};
    grpc::Status
    FetchHot(grpc::ServerContext*, const wire::FetchHotRequest*, grpc::ServerWriter<wire::FetchHotResponse>*) override
    {
        ++calls;
        return {grpc::StatusCode::FAILED_PRECONDITION, "story is tombstoned"};
    }
};
class ClusterClientTest: public ::testing::Test
{
protected:
    CatalogSnapshot catalog;
    DestroyedArchive archive;
    std::string address;
    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<grpc::Channel> channel;
    std::shared_ptr<ClusterClient> player;
    std::atomic<int> lookups{};
    std::atomic<bool> gone{}, fail_lookup{};
    void SetUp() override
    {
        grpc::ServerBuilder builder;
        int port = 0;
        rpc::applyServerPolicy(builder);
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&catalog);
        builder.RegisterService(&archive);
        server = builder.BuildAndStart();
        ASSERT_NE(server, nullptr);
        address = "127.0.0.1:" + std::to_string(port);
        channel = rpc::peerChannel(address);
        player = std::make_shared<ClusterClient>(channel,
                                                 Process{"player", "instance", "player:50054", ProcessRole::Player},
                                                 1s,
                                                 [this](StoryId id) -> absl::StatusOr<bool>
                                                 {
                                                     ++catalog.total_lookups;
                                                     catalog.cv.notify_all();
                                                     if(id == 99)
                                                         ++lookups;
                                                     if(fail_lookup)
                                                         return absl::UnavailableError("catalog unavailable");
                                                     if(id == 99)
                                                         return absl::NotFoundError("unknown story");
                                                     return gone.load();
                                                 });
    }
    void start()
    {
        ASSERT_TRUE(player->registerSelf().ok());
        ASSERT_TRUE(catalog.subscribed());
    }
    void TearDown() override
    {
        player.reset();
        server->Shutdown(std::chrono::system_clock::now() + 2s);
    }
    absl::StatusOr<RouteState> after(StoryId id, Epoch epoch)
    {
        return player->routeStateAfter(id, epoch, std::chrono::system_clock::now() + 3s);
    }
};
TEST_F(ClusterClientTest, RouteStateDoesNotRegisterPerCall)
{
    start();
    for(int n = 0; n < 1000; ++n) ASSERT_TRUE(player->routeState(42).ok());
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, RouteStateFollowsWatchedEpochChange)
{
    start();
    catalog.publish(update(42, 2, 2));
    auto state = after(42, 1);
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state->route.epoch, 2);
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, TombstonedStoryIsReportedFromTheWatch)
{
    start();
    auto removed = update(42, 0, 0);
    removed.set_tombstoned(true);
    catalog.publish(removed);
    EXPECT_EQ(after(42, 1).status().code(), absl::StatusCode::kFailedPrecondition);
    catalog.publish(update(42, 9, 9));
    catalog.publish(update(43, 1, 10));
    ASSERT_TRUE(after(43, 0).ok());
    EXPECT_EQ(player->routeState(42).status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, PreservesStateAndSameEpochPolicyChanges)
{
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(42, 2, 10), update(41, 1, 5)};
    }
    start();
    ASSERT_TRUE(player->route(41).ok());
    auto state = player->routeState(42);
    ASSERT_TRUE(state.ok());
    ASSERT_EQ(state->predecessors.size(), 1);
    EXPECT_EQ(state->predecessors[0].instance, "old-instance");
    EXPECT_EQ(state->predecessors[0].own_cut, (Hlc{200, 0}));
    EXPECT_EQ(state->predecessors[0].own_physical_ceiling_ns, 300);
    EXPECT_EQ(state->ordering_cut, (Hlc{900, 0}));
    ASSERT_EQ(state->abandoned.size(), 1);
    EXPECT_EQ(state->abandoned[0].start, (Hlc{120, 0}));
    EXPECT_TRUE(player->physicalPolicy(42));
    EXPECT_EQ(player->skewLimitNs(), 60000000000LL);
    auto changed = update(42, 2, 11);
    changed.set_physical_policy(false);
    catalog.publish(changed);
    catalog.publish(update(43, 1, 12));
    ASSERT_TRUE(after(43, 0).ok());
    EXPECT_FALSE(player->physicalPolicy(42));
    EXPECT_EQ(player->routeState(42)->archived_below, (Hlc{11, 0}));
    catalog.publish(update(42, 3, 9));
    catalog.publish(update(42, 1, 13));
    catalog.publish(update(44, 1, 14));
    ASSERT_TRUE(after(44, 0).ok());
    EXPECT_EQ(player->routeState(42)->route.epoch, 2);
    EXPECT_EQ(player->routeState(42)->archived_below, (Hlc{11, 0}));
}
TEST_F(ClusterClientTest, OverflowResubscribesAndReconcilesMissingTombstone)
{
    start();
    gone = true;
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(43, 2, 2)};
        catalog.overflow = true;
        catalog.cv.notify_all();
    }
    ASSERT_TRUE(catalog.subscribed(2));
    auto state = after(43, 0);
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state->route.epoch, 2);
    EXPECT_EQ(after(42, 1).status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, UnseenStoryUsesOneLookupAndTransientFailureIsNotDeletion)
{
    start();
    const auto before = lookups.load();
    EXPECT_EQ(player->routeState(99).status().code(), absl::StatusCode::kNotFound);
    const auto once = lookups.load();
    EXPECT_GT(once, before);
    EXPECT_EQ(player->routeState(99).status().code(), absl::StatusCode::kNotFound);
    EXPECT_EQ(lookups, once);
    fail_lookup = true;
    EXPECT_EQ(player->routeState(100).status().code(), absl::StatusCode::kUnavailable);
    fail_lookup = false;
    auto waiting = std::async(std::launch::async, [&] { return player->routeState(100); });
    catalog.publish(update(100, 1, 20));
    auto result = waiting.get();
    ASSERT_TRUE(result.ok()) << result.status();
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, ObsoleteRegistrationDoesNotReplaceNewerInstance)
{
    start();
    {
        std::lock_guard lock(catalog.mu);
        catalog.obsolete = true;
    }
    {
        std::unique_lock lock(catalog.mu);
        ASSERT_TRUE(catalog.cv.wait_for(lock, 3s, [&] { return catalog.heartbeats >= 2; }));
    }
    EXPECT_EQ(catalog.calls, 1);
}
TEST_F(ClusterClientTest, UnknownRegistrationReregistersButTransportFailureDoesNot)
{
    start();
    {
        std::lock_guard lock(catalog.mu);
        catalog.unknown = true;
    }
    {
        std::unique_lock lock(catalog.mu);
        ASSERT_TRUE(catalog.cv.wait_for(lock, 3s, [&] { return catalog.calls >= 2; }));
    }
    EXPECT_EQ(catalog.calls, 2);
    {
        std::lock_guard lock(catalog.mu);
        catalog.unavailable = true;
    }
    const int previous = catalog.heartbeats;
    {
        std::unique_lock lock(catalog.mu);
        ASSERT_TRUE(catalog.cv.wait_for(lock, 3s, [&] { return catalog.heartbeats > previous; }));
    }
    EXPECT_EQ(catalog.calls, 2);
}

TEST_F(ClusterClientTest, SlowSnapshotNeverLooksUpBeforeItsMarker)
{
    {
        std::lock_guard lock(catalog.mu);
        catalog.hold_entries = true;
        catalog.hold_marker = true;
    }
    start();
    {
        std::unique_lock lock(catalog.mu);
        EXPECT_FALSE(catalog.cv.wait_for(lock, 600ms, [&] { return catalog.total_lookups > 0; }));
        catalog.hold_entries = false;
        catalog.hold_marker = false;
        catalog.cv.notify_all();
        ASSERT_TRUE(catalog.cv.wait_for(lock, 3s, [&] { return catalog.markers >= 1; }));
    }
    ASSERT_TRUE(player->routeState(42).ok());
    EXPECT_EQ(catalog.total_lookups, 0);
}

TEST_F(ClusterClientTest, DestroyDuringDisconnectIsConcludedWithoutLookup)
{
    start();
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(43, 1, 2)};
        catalog.hold_marker = true;
        catalog.overflow = true;
        catalog.cv.notify_all();
    }
    ASSERT_TRUE(catalog.subscribed(2));
    ASSERT_TRUE(after(43, 0).ok());
    EXPECT_TRUE(player->routeState(42).ok());
    {
        std::lock_guard lock(catalog.mu);
        catalog.hold_marker = false;
        catalog.cv.notify_all();
    }
    EXPECT_EQ(after(42, 1).status().code(), absl::StatusCode::kFailedPrecondition);
    catalog.publish(update(42, 9, 9));
    catalog.publish(update(44, 1, 10));
    ASSERT_TRUE(after(44, 0).ok());
    EXPECT_EQ(player->routeState(42).status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(catalog.total_lookups, 0);
}

TEST_F(ClusterClientTest, StoryKnownAboveTheSnapshotKeepsItsRouteAndUsesLookup)
{
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(42, 1, 9)};
    }
    start();
    catalog.publish(update(42, 2, 10));
    ASSERT_TRUE(after(42, 1).ok());
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(43, 1, 5)};
        catalog.messages.clear();
        catalog.marker_revision = 5;
        catalog.overflow = true;
        catalog.cv.notify_all();
    }
    ASSERT_TRUE(catalog.subscribed(2));
    {
        std::unique_lock lock(catalog.mu);
        ASSERT_TRUE(catalog.cv.wait_for(lock, 3s, [&] { return catalog.total_lookups > 0; }));
    }
    EXPECT_EQ(player->routeState(42)->route.epoch, 2);
}

TEST_F(ClusterClientTest, RegressingSnapshotConcludesNothing)
{
    start();
    catalog.publish(update(43, 1, 9));
    ASSERT_TRUE(after(43, 0).ok());
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(43, 1, 6)};
        catalog.messages.clear();
        catalog.marker_revision = 6;
        catalog.overflow = true;
        catalog.cv.notify_all();
    }
    ASSERT_TRUE(catalog.subscribed(2));
    {
        std::unique_lock lock(catalog.mu);
        ASSERT_TRUE(catalog.cv.wait_for(lock, 3s, [&] { return catalog.total_lookups > 0; }));
    }
    EXPECT_TRUE(player->routeState(42).ok());
}

TEST_F(ClusterClientTest, StreamEndingBeforeItsMarkerReconcilesNothing)
{
    start();
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot.clear();
        catalog.marker_revision = 2;
        catalog.hold_marker = true;
        catalog.overflow = true;
        catalog.cv.notify_all();
    }
    ASSERT_TRUE(catalog.subscribed(2));
    {
        std::lock_guard lock(catalog.mu);
        catalog.overflow = true;
        catalog.cv.notify_all();
    }
    ASSERT_TRUE(catalog.subscribed(3));
    EXPECT_TRUE(player->routeState(42).ok());
    EXPECT_EQ(catalog.total_lookups, 0);
}

TEST_F(ClusterClientTest, ReadPlannedBeforeReconnectReportsSourceFailedHonestly)
{
    start();
    std::promise<void> planned, resume;
    auto resumed = resume.get_future().share();
    std::atomic<bool> signaled{};
    auto source = std::make_shared<KeeperHotSource>(
            player,
            nullptr,
            [&](const KeeperRef&)
            {
                if(!signaled.exchange(true))
                    planned.set_value();
                resumed.wait_for(5s);
                return address;
            },
            KeeperHotSourceOptions{1s});
    HotReplay replay(source);
    auto read =
            std::async(std::launch::async, [&] { return replay.read(42, {Range::Axis::Hlc, {1000, 0}, {2000, 0}}); });
    const auto reached = planned.get_future().wait_for(3s);
    {
        std::lock_guard lock(catalog.mu);
        catalog.snapshot = {update(43, 1, 2)};
        catalog.overflow = true;
        catalog.cv.notify_all();
    }
    EXPECT_EQ(after(42, 1).status().code(), absl::StatusCode::kFailedPrecondition);
    resume.set_value();
    ASSERT_EQ(reached, std::future_status::ready);
    auto stream = read.get();
    ASSERT_TRUE(stream.ok()) << stream.status();
    std::optional<Completion> completion;
    for(int batch = 0; batch < 10; ++batch)
    {
        auto next = (*stream)->next();
        ASSERT_TRUE(next.ok()) << next.status();
        if(!*next)
            break;
        if((**next).completion)
            completion = (**next).completion;
    }
    ASSERT_GT(archive.calls, 0);
    ASSERT_TRUE(completion);
    EXPECT_FALSE(completion->complete);
    EXPECT_EQ(completion->reason, IncompleteReason::SourceFailed);
}
} // namespace
} // namespace chronolog::player
