#include <atomic>
#include <array>
#include <chrono>
#include <thread>
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
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if(fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        throw std::runtime_error("port reservation failed");
    socklen_t size = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    int result = ntohs(address.sin_port);
    close(fd);
    return result;
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
        for(int i = 0; i < 3; ++i)
        {
            std::string endpoint = "127.0.0.1:" + std::to_string(port());
            peers.push_back({i + 1, endpoint, endpoint, endpoint});
        }
        for(int i = 0; i < 3; ++i)
            (*configs)[static_cast<size_t>(i)] = {i + 1, peers[static_cast<size_t>(i)].raft_endpoint, peers};
        auto open = [directory, configs, confirm](size_t i)
        {
            auto result = RaftMetadataStore::open((directory->path() / std::to_string(i)).string(),
                                                  visor::testing::twoKeeperTopology(),
                                                  (*configs)[i],
                                                  [confirm](const KeeperRef&, uint64_t) { return confirm->load(); });
            if(!result.ok())
                throw std::runtime_error(std::string(result.status().message()));
            return std::move(*result);
        };
        for(size_t i = 0; i < 3; ++i) (*replicas)[i] = open(i);
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
