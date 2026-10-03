#include <atomic>
#include <array>
#include <chrono>
#include <thread>
#include <future>
#include <sys/wait.h>
#include <signal.h>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include "TestSupport.h"
#include "raft/RaftMetadataStore.h"
#include "adapter/Convert.h"
#include "chronolog/acquire_refusal.h"
#include "metadata_store_contract_test.cpp"
namespace chronolog::contract
{
namespace
{
using visor::RaftMetadataStore;
using namespace std::chrono_literals;
int port()
{
    static int candidate = 18000 + (getpid() % 4000) * 3;
    for(int attempt = 0; attempt < 32; ++attempt)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if(fd < 0)
            throw std::runtime_error("port reservation failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int result = candidate++;
        address.sin_port = htons(static_cast<uint16_t>(result));
        int rc = bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        close(fd);
        if(rc == 0)
            return result;
    }
    throw std::runtime_error("Raft test ports unavailable");
}
std::filesystem::path startCluster(const std::filesystem::path& root,
                                   std::array<std::unique_ptr<RaftMetadataStore>, 3>& replicas,
                                   std::vector<visor::RaftPeer>& peers,
                                   visor::FenceWaiter fence = nullptr)
{
    absl::Status status;
    for(int attempt = 0; attempt < 8; ++attempt)
    {
        peers.clear();
        for(int i = 0; i < 3; ++i)
        {
            const auto endpoint = "127.0.0.1:" + std::to_string(port());
            peers.push_back({i + 1, endpoint, endpoint, endpoint});
        }
        const auto path = root / std::to_string(attempt);
        std::filesystem::create_directory(path);
        for(size_t i = 0; i < 3; ++i)
        {
            auto opened = RaftMetadataStore::open((path / std::to_string(i)).string(),
                                                  visor::testing::twoKeeperTopology(),
                                                  {static_cast<int32_t>(i + 1), peers[i].raft_endpoint, peers},
                                                  fence);
            status = opened.status();
            if(!opened.ok())
                break;
            replicas[i] = std::move(*opened);
        }
        if(status.ok())
            return path;
        for(auto& replica: replicas) replica.reset();
        if(status.message().find(" in use") == std::string_view::npos)
            break;
    }
    throw std::runtime_error(status.ToString());
}
MetadataStoreFactory factory(bool majority)
{
    return [majority]
    {
        auto directory = std::make_shared<visor::testing::TempDir>();
        auto replicas = std::make_shared<std::array<std::unique_ptr<RaftMetadataStore>, 3>>();
        auto configs = std::make_shared<std::array<visor::RaftConfig, 3>>();
        auto confirm = std::make_shared<std::atomic<bool>>(true);
        std::vector<visor::RaftPeer> peers;
        const auto path = startCluster(directory->path(),
                                       *replicas,
                                       peers,
                                       [confirm](const KeeperRef&, uint64_t) { return confirm->load(); });
        for(int i = 0; i < 3; ++i)
            (*configs)[static_cast<size_t>(i)] = {i + 1, peers[static_cast<size_t>(i)].raft_endpoint, peers};
        auto open = [directory, path, configs, confirm](size_t i)
        {
            auto result = RaftMetadataStore::open((path / std::to_string(i)).string(),
                                                  visor::testing::twoKeeperTopology(),
                                                  (*configs)[i],
                                                  [confirm](const KeeperRef&, uint64_t) { return confirm->load(); });
            if(!result.ok())
                throw std::runtime_error(std::string(result.status().message()));
            return std::move(*result);
        };
        auto select = [replicas]()
        {
            const auto until = std::chrono::steady_clock::now() + 8s;
            while(std::chrono::steady_clock::now() < until)
            {
                for(size_t i = 0; i < 3; ++i)
                    if((*replicas)[i] && (*replicas)[i]->leaderLease())
                        return i;
                std::this_thread::sleep_for(20ms);
            }
            throw std::runtime_error("Raft election timed out");
        };
        auto leader = std::make_shared<size_t>(select());
        auto h = std::make_unique<MetadataStoreHarness>();
        h->sut = std::move((*replicas)[*leader]);
        h->durable_restart = true;
        h->confirmReleaseFence = [confirm](bool enabled) { confirm->store(enabled); };
        auto* raw = h.get();
        h->restart = [raw, replicas, leader, select, open, majority]
        {
            size_t old = *leader;
            raw->sut.reset();
            if(majority)
            {
                size_t second = (old + 1) % 3;
                (*replicas)[second].reset();
                (*replicas)[old] = open(old);
                (*replicas)[second] = open(second);
            }
            *leader = select();
            raw->sut = std::move((*replicas)[*leader]);
            if(!majority)
                (*replicas)[old] = open(old);
        };
        h->acquisition_leases = true;
        h->static_fence_proof = false;
        h->lease_default_ns = 300000000000;
        h->lease_min_ns = 30000000000;
        h->lease_max_ns = 3600000000000;
        auto* lease_harness = h.get();
        h->acquireWithOptions = [lease_harness](StoryId id, std::string identity, AcquireOptions options)
        { return lease_harness->sut->acquire(id, std::move(identity), std::move(options)); };
        h->renewAcquisitions = [lease_harness](const std::vector<RenewAcquisition>& tuples)
        { return lease_harness->sut->renewAcquisitions(tuples); };
        h->advanceAuthorityClock = [lease_harness](int64_t ns, AuthorityClockMode mode)
        {
            auto* raft = dynamic_cast<RaftMetadataStore*>(lease_harness->sut.get());
            ASSERT_TRUE(raft->serviceTick().ok());
            raft->leaseAuthority().advanceClock(ns, mode == AuthorityClockMode::Ticking);
        };
        h->stepExpirySweep = [lease_harness]
        { return dynamic_cast<RaftMetadataStore*>(lease_harness->sut.get())->sweepExpiry(); };
        h->beforeNextAcquireSample = [lease_harness](std::function<void()> callback)
        {
            dynamic_cast<RaftMetadataStore*>(lease_harness->sut.get())
                    ->leaseAuthority()
                    .beforeNextSampleForTest(std::move(callback));
        };
        h->requestGrant = [lease_harness](const std::string& id)
        { return dynamic_cast<RaftMetadataStore*>(lease_harness->sut.get())->requestGrant(id); };
        return h;
    };
}
INSTANTIATE_TEST_SUITE_P(RaftLeaderLoss, MetadataStoreContract, ::testing::Values(factory(false)));
INSTANTIATE_TEST_SUITE_P(RaftMajorityRestart, MetadataStoreContract, ::testing::Values(factory(true)));
} // namespace
} // namespace chronolog::contract
namespace chronolog::visor
{
using namespace std::chrono_literals;
TEST(RaftStorageTest, SnapshotRestoresAppliedIndexAndReplayDoesNotDuplicateMutation)
{
    testing::TempDir dir;
    auto opened = SqliteMetadataStore::open((dir.path() / "catalog").string(), testing::twoKeeperTopology());
    ASSERT_TRUE(opened.ok());
    auto& store = **opened;
    ASSERT_TRUE(store.applyRaft(1, [&] { return store.createChronicle("c").status().ToString(); }).ok());
    StoryId id = 0;
    ASSERT_TRUE(store.applyRaft(2,
                                [&]
                                {
                                    auto s = store.createStory("c", "s");
                                    if(s.ok())
                                        id = s->id;
                                    return s.status().ToString();
                                })
                        .ok());
    ASSERT_NE(id, 0u);
    // The initial command carries one id; the intended successor is a fresh-id CAS against incarnation 1.
    AcquireOptions initial;
    initial.acquire_request_id = "snapshot-initial-request-00000001";
    AcquireOptions successor;
    successor.takeover = true;
    successor.expected_prior_incarnation = 1;
    successor.acquire_request_id = "snapshot-successor-request-000001";
    auto acquire = [&](const AcquireOptions& options)
    {
        auto a = store.acquireAfterFence(id, "writer", options, 300000000000);
        return a.ok() ? std::to_string(a->incarnation) : a.status().ToString();
    };
    auto first = store.applyRaft(3, [&] { return acquire(initial); });
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(*first, "1");
    auto backup = (dir.path() / "snapshot").string();
    ASSERT_TRUE(store.backupTo(backup).ok());
    auto second = store.applyRaft(4, [&] { return acquire(successor); });
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(*second, "2");
    const auto revision = store.snapshotAcquisitions()->revision;
    ASSERT_TRUE(store.installFrom(backup).ok());
    EXPECT_EQ(store.appliedIndex().value_or(0), 3u);
    bool reapplied = false;
    auto replay = store.applyRaft(3,
                                  [&]
                                  {
                                      reapplied = true;
                                      return acquire(initial);
                                  });
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(*replay, "1");
    EXPECT_FALSE(reapplied);
    second = store.applyRaft(4, [&] { return acquire(successor); });
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(*second, "2");
    EXPECT_EQ(store.snapshotAcquisitions()->revision, revision);
}

TEST(RaftStorageTest, PolicyDowngradeHistorySurvivesReplayAndSnapshot)
{
    testing::TempDir dir;
    const auto path = (dir.path() / "catalog").string();
    auto opened = SqliteMetadataStore::open(path, testing::twoKeeperTopology());
    ASSERT_TRUE(opened.ok());
    auto store = std::move(*opened);
    ASSERT_TRUE(store->registerStaticPolicy("keeper-a", 1).ok());
    ASSERT_TRUE(store->registerStaticPolicy("keeper-b", 1).ok());
    ASSERT_TRUE(store->createChronicle("c").ok());
    ASSERT_TRUE(store->createStory("c", "a").ok());
    ASSERT_TRUE(store->createStory("c", "b").ok());
    const auto cursor = store->membershipRevision().value_or(0);
    auto applied = store->applyRaft(1, [&] { return store->clearPhysicalPolicy({2, 1, 2}).ToString(); });
    ASSERT_TRUE(applied.ok());
    EXPECT_EQ(*applied, "OK");
    auto changes = store->membershipRouteChanges(cursor);
    ASSERT_TRUE(changes.ok());
    ASSERT_EQ(changes->route_history_size(), 2);
    EXPECT_EQ(changes->revision(), cursor + 1);
    for(const auto& r: changes->route_history())
    {
        EXPECT_EQ(r.revision(), cursor + 1);
        EXPECT_EQ(r.route().epoch(), 1u);
        EXPECT_FALSE(r.physical_policy());
    }
    const auto expected = changes->SerializeAsString();
    const auto snapshot = (dir.path() / "snapshot").string();
    ASSERT_TRUE(store->backupTo(snapshot).ok());
    ASSERT_TRUE(store->createStory("c", "later").ok());
    ASSERT_TRUE(store->installFrom(snapshot).ok());
    bool replayed = false;
    ASSERT_TRUE(store->applyRaft(1,
                                 [&]
                                 {
                                     replayed = true;
                                     return store->clearPhysicalPolicy({1, 2}).ToString();
                                 })
                        .ok());
    EXPECT_FALSE(replayed);
    EXPECT_EQ(store->membershipRouteChanges(cursor)->SerializeAsString(), expected);
    store.reset();
    opened = SqliteMetadataStore::open(path, testing::twoKeeperTopology());
    ASSERT_TRUE(opened.ok());
    EXPECT_EQ((*opened)->membershipRouteChanges(cursor)->SerializeAsString(), expected);
}

TEST(RaftStorageTest, DurableLogTruncationPackingAndCompactionSurviveReopen)
{
    testing::TempDir dir;
    auto path = (dir.path() / "raft").string();
    RaftConfig cfg;
    {
        DurableState log(path, cfg);
        auto entry = nuraft::cs_new<nuraft::log_entry>(7, nuraft::buffer::alloc(8));
        entry->get_buf().put(static_cast<nuraft::ulong>(123));
        entry->get_buf().pos(0);
        EXPECT_EQ(log.append(entry), 1u);
        EXPECT_EQ(log.append(entry), 2u);
        auto replacement = nuraft::cs_new<nuraft::log_entry>(8, nuraft::buffer::alloc(0));
        log.write_at(1, replacement);
        EXPECT_EQ(log.next_slot(), 2u);
        auto pack = log.pack(1, 1);
        log.apply_pack(2, *pack);
        ASSERT_TRUE(log.compact(1));
    }
    DurableState recovered(path, cfg);
    EXPECT_EQ(recovered.start_index(), 2u);
    EXPECT_EQ(recovered.next_slot(), 3u);
    EXPECT_EQ(recovered.term_at(2), 8u);
    EXPECT_EQ(recovered.entry_at(1), nullptr);
}
} // namespace chronolog::visor
namespace chronolog::visor
{
using namespace std::chrono_literals;
TEST(RaftStorageTest, LinearizableReadsAndMutationsStopWithoutQuorum)
{
    testing::TempDir directory;
    std::vector<RaftPeer> peers;
    std::array<std::unique_ptr<RaftMetadataStore>, 3> replicas;
    contract::startCluster(directory.path(), replicas, peers);
    size_t leader = 3;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while(std::chrono::steady_clock::now() < until && leader == 3)
    {
        for(size_t i = 0; i < 3; ++i)
            if(replicas[i]->leaderLease())
                leader = i;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_LT(leader, 3u);
    ASSERT_TRUE(replicas[leader]->createChronicle("c").ok());
    auto story = replicas[leader]->createStory("c", "s");
    ASSERT_TRUE(story.ok());
    auto applied = replicas[leader]->appliedStore().appliedIndex();
    ASSERT_TRUE(applied.ok());
    for(size_t i = 0; i < 3; ++i)
        if(i != leader)
            replicas[i].reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    EXPECT_FALSE(replicas[leader]->leaderLease());
    EXPECT_EQ(replicas[leader]->getStory(story->id).status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(replicas[leader]->acquire(story->id, "writer").status().code(), absl::StatusCode::kUnavailable);
    EXPECT_EQ(replicas[leader]->appliedStore().appliedIndex().value_or(0), *applied);
}
} // namespace chronolog::visor
namespace chronolog::visor
{
using namespace std::chrono_literals;
TEST(RaftStorageTest, RestartedReplicaServesNoWatchSnapshotBelowTheCommittedRevision)
{
    testing::TempDir directory;
    std::vector<RaftPeer> peers;
    std::array<std::unique_ptr<RaftMetadataStore>, 3> replicas;
    const auto path = contract::startCluster(directory.path(), replicas, peers);
    auto open = [&](size_t i)
    {
        RaftConfig config{static_cast<int32_t>(i + 1), peers[i].raft_endpoint, peers};
        auto opened =
                RaftMetadataStore::open((path / std::to_string(i)).string(), testing::twoKeeperTopology(), config);
        ASSERT_TRUE(opened.ok()) << opened.status();
        replicas[i] = std::move(*opened);
    };
    size_t leader = 3;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while(std::chrono::steady_clock::now() < until && leader == 3)
    {
        for(size_t i = 0; i < 3; ++i)
            if(replicas[i]->leaderLease())
                leader = i;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_LT(leader, 3u);
    ASSERT_TRUE(replicas[leader]->createChronicle("c").ok());
    auto story = replicas[leader]->createStory("c", "s");
    ASSERT_TRUE(story.ok());
    ASSERT_TRUE(replicas[leader]->acquire(story->id, "w0").ok());
    const size_t lagging = (leader + 1) % 3;
    const size_t other = (leader + 2) % 3;
    const auto applied = replicas[leader]->appliedStore().appliedIndex().value_or(0);
    const auto caught_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(replicas[lagging]->appliedStore().appliedIndex().value_or(0) < applied &&
          std::chrono::steady_clock::now() < caught_up)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_GE(replicas[lagging]->appliedStore().appliedIndex().value_or(0), applied);
    replicas[lagging].reset();
    for(int n = 1; n <= 5; ++n) ASSERT_TRUE(replicas[leader]->acquire(story->id, "w" + std::to_string(n)).ok());
    auto committed = replicas[leader]->snapshotAcquisitions();
    ASSERT_TRUE(committed.ok());
    replicas[leader].reset();
    replicas[other].reset();

    // Alone and behind, with no leader to tell it what is committed, it has nothing it may serve.
    open(lagging);
    const auto alone = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while(std::chrono::steady_clock::now() < alone)
    {
        EXPECT_FALSE(replicas[lagging]->appliedStateCurrent());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    open(leader);
    open(other);
    bool served = false;
    const auto rejoined = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(!served && std::chrono::steady_clock::now() < rejoined)
    {
        if(replicas[lagging]->appliedStateCurrent())
        {
            auto snapshot = replicas[lagging]->appliedStore().snapshotAcquisitions();
            ASSERT_TRUE(snapshot.ok());
            EXPECT_GE(snapshot->revision, committed->revision);
            served = true;
        }
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(served);
}
} // namespace chronolog::visor

namespace chronolog::visor
{
using namespace std::chrono_literals;
namespace
{
std::unique_ptr<RaftMetadataStore>
leaseRaft(testing::TempDir& dir, const std::shared_ptr<RaftTestControl>& control, AcquisitionLeaseConfig leases = {})
{
    const auto endpoint = "127.0.0.1:" + std::to_string(contract::port());
    RaftConfig config{1, endpoint, {{1, endpoint, endpoint, endpoint}}};
    auto opened = RaftMetadataStore::open((dir.path() / "lease-catalog").string(),
                                          testing::twoKeeperTopology(),
                                          config,
                                          nullptr,
                                          leases,
                                          control);
    if(!opened.ok())
        throw std::runtime_error(opened.status().ToString());
    auto store = std::move(*opened);
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while(!store->leaderLease() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    if(!store->leaderLease())
        throw std::runtime_error("lease test leader did not qualify");
    return store;
}
StoryId leaseStory(RaftMetadataStore& store)
{
    auto chronicle = store.createChronicle("lease");
    if(!chronicle.ok())
        throw std::runtime_error(chronicle.status().ToString());
    auto story = store.createStory("lease", "story");
    if(!story.ok())
        throw std::runtime_error(story.status().ToString());
    return story->id;
}
AcquisitionChange liveRow(RaftMetadataStore& store, const Acquisition& grant)
{
    auto rows = store.appliedStore().acquisitionRows({{grant.story_id, grant.writer_id, grant.incarnation}});
    if(!rows.ok())
        throw std::runtime_error(rows.status().ToString());
    return rows->front();
}
} // namespace

TEST(RaftStorageTest, RenewalDoesNotAppendOrBumpRevision)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    auto grant = store->acquire(story, "renew");
    ASSERT_TRUE(grant.ok());
    const auto index = store->appliedStore().appliedIndex().value_or(0);
    const auto changes = store->appliedStore().totalChanges();
    const auto revision = store->snapshotAcquisitions()->revision;
    const auto proposals = control->proposal_count.load();
    store->leaseAuthority().advanceClock(1000000000, true);
    auto renewed = store->renewAcquisitions({{story, grant->writer_id, grant->incarnation}, {story, 99999, 1}});
    ASSERT_TRUE(renewed.ok());
    EXPECT_TRUE(renewed->front().status.ok());
    EXPECT_TRUE(absl::IsNotFound(renewed->back().status));
    EXPECT_EQ(store->appliedStore().totalChanges(), changes);
    EXPECT_EQ(store->appliedStore().appliedIndex().value_or(0), index);
    EXPECT_EQ(store->snapshotAcquisitions()->revision, revision);
    EXPECT_EQ(control->proposal_count.load(), proposals);
}

TEST(RaftStorageTest, MissingLiveDeadlineIsReconciled)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    AcquireOptions options;
    options.acquire_request_id = "missing-map-request-000000000001";
    auto grant = store->acquire(story, "missing", options);
    ASSERT_TRUE(grant.ok());
    const RenewAcquisition tuple{story, grant->writer_id, grant->incarnation};
    for(int path = 0; path < 5; ++path)
    {
        store->leaseAuthority().eraseForTest(tuple);
        EXPECT_EQ(store->leaseAuthority().size(), 0u);
        if(path == 0)
        {
            ASSERT_TRUE(store->reconcileLeases().ok());
        }
        if(path == 1)
        {
            ASSERT_TRUE(store->acquire(story, "missing", options).ok());
        }
        if(path == 2)
        {
            auto renewal = store->renewAcquisitions({tuple});
            ASSERT_TRUE(renewal.ok());
            ASSERT_TRUE(renewal->front().status.ok());
        }
        if(path == 3)
        {
            auto renewed = store->acceptKeeperEvidence(grant->assigned_keeper.process_id, {tuple});
            ASSERT_TRUE(renewed.ok());
            EXPECT_EQ(*renewed, 1u);
        }
        if(path == 4)
        {
            // A HELD sample of a different logical Acquire initializes the missing deadline too.
            AcquireOptions other;
            other.acquire_request_id = "missing-map-held-request-0000001";
            auto held = store->acquire(story, "missing", other);
            ASSERT_TRUE(absl::IsFailedPrecondition(held.status())) << held.status();
            auto refusal = getAcquireRefusal(held.status());
            ASSERT_TRUE(refusal);
            EXPECT_EQ(refusal->refusal_reason, AcquireRefusalReason::Held);
            EXPECT_GT(refusal->remaining_ns, grant->lease.duration_ns * 9 / 10);
        }
        EXPECT_EQ(store->leaseAuthority().size(), 1u);
    }
    auto row = liveRow(*store, *grant);
    store->leaseAuthority().advanceClock(grant->lease.duration_ns + 1, true);
    auto due = store->leaseAuthority().sample(row, false);
    ASSERT_TRUE(due.ok());
    EXPECT_EQ(due->remaining_ns, 0);
    ASSERT_TRUE(store->reconcileLeases().ok());
    due = store->leaseAuthority().sample(row, false);
    ASSERT_TRUE(due.ok());
    EXPECT_EQ(due->remaining_ns, 0);
    auto renewal = store->renewAcquisitions({tuple});
    ASSERT_TRUE(renewal.ok());
    EXPECT_TRUE(absl::IsUnavailable(renewal->front().status));
    auto evidence = store->acceptKeeperEvidence(grant->assigned_keeper.process_id, {tuple});
    ASSERT_TRUE(evidence.ok());
    EXPECT_EQ(*evidence, 0u);
    due = store->leaseAuthority().sample(row, false);
    ASSERT_TRUE(due.ok());
    EXPECT_EQ(due->remaining_ns, 0);
    ASSERT_TRUE(store->release(story, grant->writer_id, grant->incarnation).ok());
    EXPECT_EQ(store->leaseAuthority().size(), 0u);
    EXPECT_EQ(store->snapshotAcquisitions()->active.size(), 0u);
}

TEST(RaftStorageTest, IntraTermLeaseLapsePausesDeadlinesWithoutReset)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    auto grant = store->acquire(story, "pause");
    ASSERT_TRUE(grant.ok());
    auto row = liveRow(*store, *grant);
    store->leaseAuthority().advanceClock(grant->lease.duration_ns * 9 / 10, true);
    auto before = store->leaseAuthority().sample(row, false);
    ASSERT_TRUE(before.ok());
    const auto term = store->term();
    const auto proposals = control->proposal_count.load();
    for(int lapse = 0; lapse < 2; ++lapse)
    {
        control->qualification_enabled = false;
        auto refused = store->renewAcquisitions({{story, grant->writer_id, grant->incarnation}});
        EXPECT_TRUE(absl::IsUnavailable(refused.status()));
        EXPECT_TRUE(absl::IsUnavailable(store->acquire(story, "refused").status()));
        store->leaseAuthority().advanceClock(grant->lease.duration_ns * 2, true);
        EXPECT_TRUE(absl::IsUnavailable(store->serviceTick()));
        control->qualification_enabled = true;
        ASSERT_TRUE(store->serviceTick().ok());
        auto remaining = store->leaseAuthority().sample(row, false);
        ASSERT_TRUE(remaining.ok());
        EXPECT_LT(remaining->remaining_ns, grant->lease.duration_ns / 5);
        EXPECT_GT(remaining->remaining_ns, before->remaining_ns - 1000000000);
        EXPECT_EQ(store->term(), term);
    }
    EXPECT_EQ(control->proposal_count.load(), proposals);
}

TEST(RaftStorageTest, TimedOutAcquireThatCommitsLaterReturnsTheSameGrant)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    AcquireOptions options;
    options.acquire_request_id = "late-retry-request-000000000001";
    control->holdNextApply();
    auto proposal = std::async(std::launch::async, [&] { return store->acquire(story, "late", options); });
    const auto accepted = control->waitAccepted(2s);
    EXPECT_NE(accepted, 0u);
    const auto waited = proposal.wait_for(5s);
    if(waited != std::future_status::ready)
        control->releaseApply();
    ASSERT_EQ(waited, std::future_status::ready);
    auto timed_out = proposal.get();
    EXPECT_TRUE(absl::IsUnavailable(timed_out.status()));
    control->releaseApply();
    const auto until = std::chrono::steady_clock::now() + 3s;
    while(store->appliedStore().publishedAppliedIndex() < accepted && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(5ms);
    ASSERT_GE(store->appliedStore().publishedAppliedIndex(), accepted);
    auto committed = store->requestGrant(options.acquire_request_id);
    ASSERT_TRUE(committed.ok());
    const auto revision = store->snapshotAcquisitions()->revision;
    auto retry = store->acquire(story, "late", options);
    ASSERT_TRUE(retry.ok()) << retry.status();
    EXPECT_EQ(retry->incarnation, committed->incarnation);
    EXPECT_EQ(retry->assigned_keeper, committed->assigned_keeper);
    EXPECT_EQ(retry->route, committed->route);
    EXPECT_EQ(store->snapshotAcquisitions()->revision, revision);
}

TEST(RaftStorageTest, LateCommittedGrantWithoutRetryStillExpires)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    for(int kind = 0; kind < 2; ++kind)
    {
        AcquireOptions options;
        options.acquire_request_id = "late-no-retry-00000000000000001" + std::to_string(kind);
        options.takeover = kind == 1;
        if(kind == 1)
            options.expected_prior_incarnation = 1;
        control->holdNextApply();
        auto proposal = std::async(std::launch::async, [&] { return store->acquire(story, "silent", options); });
        const auto accepted = control->waitAccepted(2s);
        EXPECT_NE(accepted, 0u);
        const auto waited = proposal.wait_for(5s);
        if(waited != std::future_status::ready)
            control->releaseApply();
        ASSERT_EQ(waited, std::future_status::ready);
        EXPECT_TRUE(absl::IsUnavailable(proposal.get().status()));
        control->releaseApply();
        const auto until = std::chrono::steady_clock::now() + 3s;
        while(store->appliedStore().publishedAppliedIndex() < accepted && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(5ms);
        ASSERT_GE(store->appliedStore().publishedAppliedIndex(), accepted);
        auto grant = store->requestGrant(options.acquire_request_id);
        ASSERT_TRUE(grant.ok());
        auto row = liveRow(*store, *grant);
        ASSERT_EQ(store->leaseAuthority().size(), 1u);
        ASSERT_TRUE(store->leaderLease());
        auto installed = store->leaseAuthority().sample(row, false);
        ASSERT_TRUE(installed.ok());
        EXPECT_GT(installed->remaining_ns, grant->lease.duration_ns * 9 / 10);
        store->leaseAuthority().advanceClock(grant->lease.duration_ns + 1, true);
        auto due = store->leaseAuthority().sample(row, false);
        ASSERT_TRUE(due.ok());
        EXPECT_EQ(due->remaining_ns, 0);
        auto selected_due = store->leaseAuthority().dueTuples(1);
        ASSERT_TRUE(selected_due.ok());
        ASSERT_EQ(selected_due->size(), 1u);
        EXPECT_EQ(selected_due->front().incarnation, grant->incarnation);
        EXPECT_EQ(store->snapshotAcquisitions()->active.size(), 1u);
        // The sweep commits EXPIRED for the never-retried grant through the normal proposal path.
        ASSERT_TRUE(store->sweepExpiry().ok());
        EXPECT_TRUE(store->snapshotAcquisitions()->active.empty());
        auto cause = store->renewAcquisitions({{story, grant->writer_id, grant->incarnation}});
        ASSERT_TRUE(cause.ok());
        EXPECT_EQ(cause->front().termination_cause, AcquisitionTerminationCause::Expired);
        EXPECT_EQ(store->leaseAuthority().size(), 0u);
    }
    // Neither late grant pins destroy any longer.
    EXPECT_TRUE(store->destroyStory(story).ok());
}

TEST(RaftStorageTest, ConcurrentPlainAcquiresNeverSupersedeALiveHolder)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    // Both proposals are prepared before either applies, so both carry the same absent predecessor.
    internal::v1::CatalogCommand commands[2];
    for(int i = 0; i < 2; ++i)
    {
        v1::AcquireRequest request;
        request.set_story_id(story);
        request.set_writer_identity("contended");
        request.set_acquire_request_id("contended-request-0000000000000" + std::to_string(i));
        auto selected = store->appliedStore().prepareAcquire(request, 300000000000);
        ASSERT_TRUE(selected.ok());
        EXPECT_FALSE(selected->has_prior_incarnation());
        *commands[i].mutable_acquire_with_lease() = *selected;
    }
    auto first = store->propose(commands[0]);
    ASSERT_TRUE(first.ok());
    v1::AcquireResponse granted;
    ASSERT_TRUE(granted.ParseFromString(*first));
    ASSERT_EQ(granted.status().code(), 0);
    EXPECT_EQ(granted.incarnation(), 1u);
    const auto revision = store->snapshotAcquisitions()->revision;
    auto second = store->propose(commands[1]);
    ASSERT_TRUE(second.ok());
    v1::AcquireResponse refused;
    ASSERT_TRUE(refused.ParseFromString(*second));
    EXPECT_EQ(refused.status().code(), static_cast<int>(absl::StatusCode::kFailedPrecondition));
    EXPECT_EQ(refused.refusal_reason(), v1::ACQUIRE_REFUSAL_REASON_HELD);
    EXPECT_EQ(store->snapshotAcquisitions()->revision, revision);
    auto snapshot = store->snapshotAcquisitions();
    ASSERT_EQ(snapshot->active.size(), 1u);
    EXPECT_EQ(snapshot->active.front().incarnation, 1u);
    // Through the store, the leader supplies the HELD remainder after apply.
    AcquireOptions third;
    third.acquire_request_id = "contended-request-00000000000002";
    auto held = store->acquire(story, "contended", third);
    ASSERT_TRUE(absl::IsFailedPrecondition(held.status()));
    auto refusal = getAcquireRefusal(held.status());
    ASSERT_TRUE(refusal);
    EXPECT_EQ(refusal->refusal_reason, AcquireRefusalReason::Held);
    EXPECT_GT(refusal->remaining_ns, 0);
    EXPECT_EQ(store->snapshotAcquisitions()->revision, revision);
}

TEST(RaftStorageTest, NoExpiryProposalWithoutQualifiedLeader)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    auto grant = store->acquire(story, "unqualified");
    ASSERT_TRUE(grant.ok());
    store->leaseAuthority().advanceClock(grant->lease.duration_ns + 1, true);
    const auto proposals = control->proposal_count.load();
    control->qualification_enabled = false;
    EXPECT_TRUE(absl::IsUnavailable(store->sweepExpiry()));
    EXPECT_TRUE(absl::IsUnavailable(store->serviceTick()));
    EXPECT_TRUE(absl::IsUnavailable(store->destroyStory(story)));
    EXPECT_EQ(control->proposal_count.load(), proposals);
    EXPECT_EQ(store->snapshotAcquisitions()->active.size(), 1u);
    control->qualification_enabled = true;
    ASSERT_TRUE(store->serviceTick().ok());
    EXPECT_EQ(control->proposal_count.load(), proposals + 1);
    EXPECT_TRUE(store->snapshotAcquisitions()->active.empty());
}

TEST(RaftStorageTest, RenewDoesNoReconciliationScan)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    auto grant = store->acquire(story, "renewing");
    ASSERT_TRUE(grant.ok());
    const RenewAcquisition tuple{story, grant->writer_id, grant->incarnation};
    ASSERT_TRUE(store->serviceTick().ok());
    const auto scans = store->leaseAuthority().reconciliations();
    for(int call = 0; call < 5; ++call)
    {
        auto renewed = store->renewAcquisitions({tuple});
        ASSERT_TRUE(renewed.ok());
        EXPECT_TRUE(renewed->front().status.ok());
    }
    EXPECT_EQ(store->leaseAuthority().reconciliations(), scans);
    store->leaseAuthority().eraseForTest(tuple);
    auto installed = store->renewAcquisitions({tuple});
    ASSERT_TRUE(installed.ok());
    EXPECT_TRUE(installed->front().status.ok());
    EXPECT_EQ(store->leaseAuthority().size(), 1u);
    EXPECT_EQ(store->leaseAuthority().reconciliations(), scans);
    ASSERT_TRUE(store->serviceTick().ok());
    EXPECT_GT(store->leaseAuthority().reconciliations(), scans);
}

TEST(RaftStorageTest, CommittedNoOpExpiryResolvesItsSelection)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    auto grant = store->acquire(story, "no-op");
    ASSERT_TRUE(grant.ok());
    store->leaseAuthority().advanceClock(grant->lease.duration_ns + 1, true);
    auto selected = store->leaseAuthority().dueTuples(8);
    ASSERT_TRUE(selected.ok());
    ASSERT_EQ(selected->size(), 1u);
    // A first ExpireAcquisitions commits; a duplicate of the same tuple commits as a typed no-op outcome.
    for(int round = 0; round < 2; ++round)
    {
        internal::v1::CatalogCommand command;
        auto* tuple = command.mutable_expire_acquisitions()->add_acquisitions();
        tuple->set_story_id(story);
        tuple->set_writer_id(grant->writer_id);
        tuple->set_incarnation(grant->incarnation);
        auto result = store->propose(command);
        ASSERT_TRUE(result.ok());
        v1::RenewAcquisitionsResponse outcomes;
        ASSERT_TRUE(outcomes.ParseFromString(*result));
        ASSERT_EQ(outcomes.results_size(), 1);
        EXPECT_EQ(outcomes.results(0).status().code(),
                  round ? static_cast<int>(absl::StatusCode::kFailedPrecondition) : 0);
        EXPECT_EQ(outcomes.results(0).termination_cause(), v1::ACQUISITION_TERMINATION_CAUSE_EXPIRED);
    }
    EXPECT_FALSE(store->leaseAuthority().pending(selected->front()));
    EXPECT_EQ(store->leaseAuthority().size(), 0u);
    auto empty = store->leaseAuthority().dueTuples(8);
    ASSERT_TRUE(empty.ok());
    EXPECT_TRUE(empty->empty());
}

TEST(RaftStorageTest, NewLeaderRebuildBuffersTransitionsAfterCapturedAppliedIndex)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    control->holdNextRebuild();
    auto rebuild = std::async(std::launch::async, [&] { return store->serviceTick(); });
    const bool captured = control->waitRebuild(2s);
    if(!captured)
        control->releaseRebuild();
    ASSERT_TRUE(captured);
    v1::AcquireRequest request;
    request.set_story_id(story);
    request.set_writer_identity("step-up");
    request.set_acquire_request_id("step-up-request-000000000000001");
    auto selected = store->appliedStore().prepareAcquire(request, 300000000000);
    ASSERT_TRUE(selected.ok());
    internal::v1::CatalogCommand command;
    *command.mutable_acquire_with_lease() = *selected;
    auto committed = store->propose(command);
    EXPECT_TRUE(committed.ok());
    const auto applied = store->appliedStore().publishedAppliedIndex();
    EXPECT_GT(applied, control->captured_index);
    control->releaseRebuild();
    ASSERT_TRUE(rebuild.get().ok());
    EXPECT_EQ(store->leaseAuthority().size(), 1u);
    auto grant = store->requestGrant(request.acquire_request_id());
    ASSERT_TRUE(grant.ok());
    auto row = liveRow(*store, *grant);
    EXPECT_TRUE(store->leaseAuthority().sample(row, false).ok());
}

TEST(RaftStorageTest, RetryIdSynthesisRunsOnlyAtProposer)
{
    testing::TempDir dir;
    auto control = std::make_shared<RaftTestControl>();
    auto store = leaseRaft(dir, control);
    const auto story = leaseStory(*store);
    control->suppress_reply = true;
    auto lost = store->acquire(story, "synthesized");
    EXPECT_TRUE(absl::IsUnavailable(lost.status()));
    auto snapshot = store->snapshotAcquisitions();
    ASSERT_TRUE(snapshot.ok());
    ASSERT_EQ(snapshot->active.size(), 1u);
    EXPECT_EQ(snapshot->active.front().incarnation, 1u);
    EXPECT_EQ(control->proposal_count.load(), 3u);
    internal::v1::CatalogCommand serialized;
    ASSERT_TRUE(serialized.ParseFromString(control->lastCommand()));
    ASSERT_EQ(serialized.mutation_case(), internal::v1::CatalogCommand::kAcquireWithLease);
    const auto& carried = serialized.acquire_with_lease();
    EXPECT_EQ(carried.lease_duration_ns(), 300000000000);
    EXPECT_EQ(carried.request().acquire_request_id().size(), 32u);
    auto grant = store->requestGrant(carried.request().acquire_request_id());
    ASSERT_TRUE(grant.ok());
    EXPECT_EQ(grant->incarnation, 1u);
}
} // namespace chronolog::visor

namespace chronolog::visor
{
using namespace std::chrono_literals;
TEST(RaftStorageTest, LeaseCommandReplayIsDeterministic)
{
    testing::TempDir first_dir, second_dir;
    AcquisitionLeaseConfig alternate;
    alternate.acquisition_lease_default_ns = 900000000000;
    auto first = SqliteMetadataStore::open((first_dir.path() / "catalog").string(),
                                           testing::twoKeeperTopology(),
                                           nullptr,
                                           {},
                                           true);
    auto second = SqliteMetadataStore::open((second_dir.path() / "catalog").string(),
                                            testing::twoKeeperTopology(),
                                            nullptr,
                                            alternate,
                                            true);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    std::string responses[2];
    int replica = 0;
    for(auto* store: {first->get(), second->get()})
    {
        store->leaseAuthority().advanceClock(replica ? 600000000000 : 0, false);
        ASSERT_TRUE(store->applyRaft(1, [&] { return store->createChronicle("replay").status().ToString(); }).ok());
        ASSERT_TRUE(
                store->applyRaft(2, [&] { return store->createStory("replay", "story").status().ToString(); }).ok());
        v1::AcquireRequest request;
        request.set_story_id(1);
        request.set_writer_identity("replay");
        request.set_acquire_request_id("replay-request-0000000000000001");
        auto command = store->prepareAcquire(request, 300000000000);
        ASSERT_TRUE(command.ok());
        auto applied = store->applyRaft(3,
                                        [&]
                                        {
                                            auto grant = store->acquireAfterFence(
                                                    1,
                                                    "replay",
                                                    convert::fromAcquireRequest(command->request()),
                                                    command->lease_duration_ns(),
                                                    &*command);
                                            if(!grant.ok())
                                                return grant.status().ToString();
                                            return convert::toAcquireResponse(*grant).SerializeAsString();
                                        });
        ASSERT_TRUE(applied.ok());
        responses[replica++] = *applied;
        const auto revision = store->snapshotAcquisitions()->revision;
        auto duplicate = store->applyRaft(4,
                                          [&]
                                          {
                                              auto grant = store->acquireAfterFence(
                                                      1,
                                                      "replay",
                                                      convert::fromAcquireRequest(command->request()),
                                                      command->lease_duration_ns(),
                                                      &*command);
                                              if(!grant.ok())
                                                  return grant.status().ToString();
                                              return convert::toAcquireResponse(*grant).SerializeAsString();
                                          });
        ASSERT_TRUE(duplicate.ok());
        EXPECT_EQ(*duplicate, *applied);
        EXPECT_EQ(store->snapshotAcquisitions()->revision, revision);
        EXPECT_EQ(store->leaseAuthority().size(), 0u);
    }
    EXPECT_EQ(responses[0], responses[1]);
}

TEST(RaftStorageTest, ExpiryReplayUsesNoReplicaClock)
{
    testing::TempDir first_dir, second_dir;
    auto first = SqliteMetadataStore::open((first_dir.path() / "catalog").string(),
                                           testing::twoKeeperTopology(),
                                           nullptr,
                                           {},
                                           true);
    auto second = SqliteMetadataStore::open((second_dir.path() / "catalog").string(),
                                            testing::twoKeeperTopology(),
                                            nullptr,
                                            {},
                                            true);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    std::string results[2][3];
    int replica = 0;
    for(auto* store: {first->get(), second->get()})
    {
        // Replicas sit at different clock origins; apply never samples them.
        store->leaseAuthority().advanceClock(replica ? 900000000000 : 0, false);
        ASSERT_TRUE(store->applyRaft(1, [&] { return store->createChronicle("replay").status().ToString(); }).ok());
        ASSERT_TRUE(store->applyRaft(2, [&] { return store->createStory("replay", "a").status().ToString(); }).ok());
        ASSERT_TRUE(store->applyRaft(3, [&] { return store->createStory("replay", "b").status().ToString(); }).ok());
        auto grant = [&](StoryId story, const std::string& identity)
        {
            AcquireOptions options;
            options.acquire_request_id = "replay-" + identity + "-000000000000000001";
            auto granted = store->acquireAfterFence(story, identity, options, 300000000000);
            return granted.ok() ? std::to_string(granted->incarnation) : granted.status().ToString();
        };
        ASSERT_EQ(*store->applyRaft(4, [&] { return grant(1, "x"); }), "1");
        ASSERT_EQ(*store->applyRaft(5, [&] { return grant(2, "y"); }), "1");
        ASSERT_EQ(*store->applyRaft(6, [&] { return grant(2, "z"); }), "1");
        auto expire = [&](const std::vector<RenewAcquisition>& tuples)
        {
            v1::RenewAcquisitionsResponse response;
            auto outcomes = store->expireAcquisitions(tuples);
            for(const auto& outcome: *outcomes)
            {
                auto* item = response.add_results();
                *item->mutable_status() = convert::toProto(outcome.status);
                if(outcome.termination_cause)
                    item->set_termination_cause(
                            static_cast<v1::AcquisitionTerminationCause>(*outcome.termination_cause));
            }
            return response.SerializeAsString();
        };
        results[replica][0] = *store->applyRaft(7, [&] { return expire({{1, 1, 1}, {1, 1, 1}, {9, 9, 9}}); });
        results[replica][1] =
                *store->applyRaft(8, [&] { return store->destroyStoryWithDue(2, {{2, 2, 1}}).ToString(); });
        results[replica][2] =
                *store->applyRaft(9, [&] { return store->destroyChronicleWithDue("replay", {{2, 3, 1}}).ToString(); });
        bool reapplied = false;
        EXPECT_EQ(*store->applyRaft(7,
                                    [&]
                                    {
                                        reapplied = true;
                                        return std::string();
                                    }),
                  results[replica][2]);
        EXPECT_FALSE(reapplied);
        EXPECT_EQ(store->leaseAuthority().size(), 0u);
        EXPECT_TRUE(store->snapshotAcquisitions()->active.empty());
        EXPECT_TRUE(store->getStory(1)->tombstoned);
        ++replica;
    }
    for(int i = 0; i < 3; ++i) EXPECT_EQ(results[0][i], results[1][i]);
    // The batch expired x once (duplicate is a no-op, unknown NOT_FOUND); destroy of b refused on z, then the
    // chronicle wrapper materialized z and tombstoned everything.
    EXPECT_EQ(results[0][1].substr(0, 19), "FAILED_PRECONDITION");
    EXPECT_EQ(results[0][2], "OK");
}
} // namespace chronolog::visor

namespace chronolog::visor
{
TEST(RaftStorageTest, RealServiceStallPausesDeadlines)
{
    int ready[2], resume[2];
    ASSERT_EQ(pipe(ready), 0);
    ASSERT_EQ(pipe(resume), 0);
    const auto child = fork();
    ASSERT_GE(child, 0);
    if(!child)
    {
        close(ready[0]);
        close(resume[1]);
        testing::TempDir dir;
        auto control = std::make_shared<RaftTestControl>();
        AcquisitionLeaseConfig config;
        config.acquisition_lease_min_ns = 100000000;
        config.acquisition_lease_default_ns = 100000000;
        config.acquisition_lease_max_ns = 100000000;
        config.acquisition_service_tick_ms = 2;
        config.acquisition_service_gap_ms = 20;
        auto store = leaseRaft(dir, control, config);
        const auto story = leaseStory(*store);
        auto grant = store->acquire(story, "real-stall");
        if(!grant.ok() || !store->serviceTick().ok())
            _exit(2);
        const char marker = 'r';
        if(write(ready[1], &marker, 1) != 1)
            _exit(3);
        char signal;
        if(read(resume[0], &signal, 1) != 1)
            _exit(4);
        auto renewal = store->renewAcquisitions({{story, grant->writer_id, grant->incarnation}});
        if(!renewal.ok() || !renewal->front().status.ok())
            _exit(5);
        for(int i = 0; i < 70; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if(!store->serviceTick().ok())
                _exit(6);
        }
        // Continuous service then commits the dead holder's expiry.
        auto expired = store->renewAcquisitions({{story, grant->writer_id, grant->incarnation}});
        if(!expired.ok() || !absl::IsFailedPrecondition(expired->front().status) ||
           expired->front().termination_cause != AcquisitionTerminationCause::Expired)
            _exit(7);
        store.reset();
        _exit(0);
    }
    close(ready[1]);
    close(resume[0]);
    char marker;
    ASSERT_EQ(read(ready[0], &marker, 1), 1);
    ASSERT_EQ(kill(child, SIGSTOP), 0);
    int status{};
    ASSERT_EQ(waitpid(child, &status, WUNTRACED), child);
    ASSERT_TRUE(WIFSTOPPED(status));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    ASSERT_EQ(kill(child, SIGCONT), 0);
    ASSERT_EQ(write(resume[1], &marker, 1), 1);
    ASSERT_EQ(waitpid(child, &status, 0), child);
    close(ready[0]);
    close(resume[1]);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
} // namespace chronolog::visor

namespace chronolog::visor
{
namespace
{
struct FsyncBarrier;
FsyncBarrier* active_fsync_barrier{};
struct FsyncBarrier
{
    struct File
    {
        const sqlite3_io_methods* original;
        sqlite3_io_methods methods;
        bool catalog;
    };
    sqlite3_vfs* original = sqlite3_vfs_find(nullptr);
    sqlite3_vfs vfs = *original;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<sqlite3_file*, File> files;
    bool armed{}, hit{}, held{};
    FsyncBarrier()
    {
        active_fsync_barrier = this;
        vfs.zName = "g2-fsync-barrier";
        vfs.xOpen = open;
        if(sqlite3_vfs_register(&vfs, 1) != SQLITE_OK)
            throw std::runtime_error("fsync test VFS registration failed");
    }
    ~FsyncBarrier()
    {
        release();
        sqlite3_vfs_register(original, 1);
        sqlite3_vfs_unregister(&vfs);
        active_fsync_barrier = nullptr;
    }
    static int open(sqlite3_vfs*, const char* name, sqlite3_file* file, int flags, int* output)
    {
        auto& barrier = *active_fsync_barrier;
        const int result = barrier.original->xOpen(barrier.original, name, file, flags, output);
        if(result != SQLITE_OK || !file->pMethods)
            return result;
        std::lock_guard lock(barrier.mutex);
        auto& wrapped = barrier.files
                                .emplace(file,
                                         File{file->pMethods,
                                              *file->pMethods,
                                              name && std::string_view(name).find("lease-catalog-wal") !=
                                                              std::string_view::npos})
                                .first->second;
        wrapped.methods.xSync = sync;
        wrapped.methods.xClose = close;
        file->pMethods = &wrapped.methods;
        return result;
    }
    static int close(sqlite3_file* file)
    {
        auto& barrier = *active_fsync_barrier;
        const sqlite3_io_methods* methods;
        {
            std::lock_guard lock(barrier.mutex);
            methods = barrier.files.at(file).original;
        }
        const auto result = methods->xClose(file);
        {
            std::lock_guard lock(barrier.mutex);
            barrier.files.erase(file);
        }
        return result;
    }
    static int sync(sqlite3_file* file, int flags)
    {
        auto& barrier = *active_fsync_barrier;
        std::unique_lock lock(barrier.mutex);
        const auto wrapped = barrier.files.at(file);
        if(barrier.armed && wrapped.catalog)
        {
            barrier.armed = false;
            barrier.hit = true;
            barrier.cv.notify_all();
            if(!barrier.cv.wait_for(lock, 3s, [&] { return !barrier.held; }))
                return SQLITE_IOERR_FSYNC;
        }
        lock.unlock();
        return wrapped.original->xSync(file, flags);
    }
    void arm()
    {
        std::lock_guard lock(mutex);
        armed = true;
        held = true;
        hit = false;
    }
    bool wait()
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 2s, [&] { return hit; });
    }
    void release()
    {
        std::lock_guard lock(mutex);
        held = false;
        cv.notify_all();
    }
};
} // namespace

TEST(RaftStorageTest, BlockedSqliteFsyncPausesDeadlinesWithoutHoldingMapLock)
{
    testing::TempDir dir;
    FsyncBarrier barrier;
    auto control = std::make_shared<RaftTestControl>();
    AcquisitionLeaseConfig config;
    config.acquisition_lease_min_ns = 100000000;
    config.acquisition_lease_default_ns = 100000000;
    config.acquisition_lease_max_ns = 100000000;
    config.acquisition_service_tick_ms = 2;
    config.acquisition_service_gap_ms = 20;
    auto store = leaseRaft(dir, control, config);
    const auto story = leaseStory(*store);
    auto grant = store->acquire(story, "before-fsync");
    ASSERT_TRUE(grant.ok());
    auto row = liveRow(*store, *grant);
    ASSERT_TRUE(store->serviceTick().ok());
    barrier.arm();
    auto blocked = std::async(std::launch::async, [&] { return store->acquire(story, "blocked-fsync"); });
    const bool reached = barrier.wait();
    if(!reached)
        barrier.release();
    ASSERT_TRUE(reached);
    auto tick = std::async(std::launch::async, [&] { return store->serviceTick(); });
    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(store->leaderLease());
    // Reading qualification and the map succeeds while SQLite's actual xSync is held.
    auto preserved = store->leaseAuthority().sample(row, false);
    EXPECT_TRUE(preserved.ok()) << preserved.status();
    if(preserved.ok())
    {
        EXPECT_GT(preserved->remaining_ns, 50000000);
    }
    barrier.release();
    EXPECT_TRUE(blocked.get().ok());
    EXPECT_TRUE(tick.get().ok());
    auto renewed = store->renewAcquisitions({{story, grant->writer_id, grant->incarnation}});
    ASSERT_TRUE(renewed.ok());
    ASSERT_TRUE(renewed->front().status.ok()) << renewed->front().status;
    for(int i = 0; i < 70; ++i)
    {
        std::this_thread::sleep_for(2ms);
        ASSERT_TRUE(store->serviceTick().ok());
    }
    // Continuous service then commits the dead holder's expiry.
    auto expired = store->renewAcquisitions({{story, grant->writer_id, grant->incarnation}});
    ASSERT_TRUE(expired.ok());
    EXPECT_TRUE(absl::IsFailedPrecondition(expired->front().status)) << expired->front().status;
    EXPECT_EQ(expired->front().termination_cause, AcquisitionTerminationCause::Expired);
}
} // namespace chronolog::visor
