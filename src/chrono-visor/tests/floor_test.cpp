#include "TestSupport.h"
#include "dynamic/MembershipState.h"
#include "raft/RaftMetadataStore.h"
#include "clock/FakeClock.h"
#include "journal/RamJournal.h"
#include "membership/ConfigMembership.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
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
            auto opened = visor::RaftMetadataStore::open((dir.path() / "raft").string(),
                                                         visor::testing::twoKeeperTopology(),
                                                         {1, endpoint, {{1, endpoint, endpoint, endpoint}}});
            ASSERT_TRUE(opened.ok()) << opened.status();
            raft = std::move(*opened);
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
} // namespace chronolog::floor_contract
