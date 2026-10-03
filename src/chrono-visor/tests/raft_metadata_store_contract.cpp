#include <atomic>
#include <array>
#include <chrono>
#include <thread>
#include <string_view>
#include <sys/socket.h>
#include <netinet/in.h>
#include "TestSupport.h"
#include "raft/RaftMetadataStore.h"
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
        return h;
    };
}
INSTANTIATE_TEST_SUITE_P(RaftLeaderLoss, MetadataStoreContract, ::testing::Values(factory(false)));
INSTANTIATE_TEST_SUITE_P(RaftMajorityRestart, MetadataStoreContract, ::testing::Values(factory(true)));
} // namespace
} // namespace chronolog::contract
namespace chronolog::visor
{
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
    auto acquire = [&]
    {
        auto a = store.acquire(id, "writer");
        return a.ok() ? std::to_string(a->incarnation) : a.status().ToString();
    };
    auto first = store.applyRaft(3, acquire);
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(*first, "1");
    auto backup = (dir.path() / "snapshot").string();
    ASSERT_TRUE(store.backupTo(backup).ok());
    auto second = store.applyRaft(4, acquire);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(*second, "2");
    ASSERT_TRUE(store.installFrom(backup).ok());
    EXPECT_EQ(store.appliedIndex().value_or(0), 3u);
    bool reapplied = false;
    auto replay = store.applyRaft(3,
                                  [&]
                                  {
                                      reapplied = true;
                                      return acquire();
                                  });
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(*replay, "1");
    EXPECT_FALSE(reapplied);
    second = store.applyRaft(4, acquire);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(*second, "2");
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
