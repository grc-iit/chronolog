#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>
#include "TestSupport.h"
#include "dynamic/MembershipState.h"
#include "raft/RaftMetadataStore.h"

namespace chronolog::visor
{
TEST(DynamicBootstrapTest, RegistrationBeforeFirstStoryPreservesConfiguredOwners)
{
    testing::TempDir dir;
    auto store = SqliteMetadataStore::open((dir.path() / "catalog").string(), testing::twoKeeperTopology());
    ASSERT_TRUE(store.ok());
    for(const auto& id: {"keeper-a", "keeper-b", "keeper-extra"})
    {
        internal::v1::MembershipCommand command;
        auto* request = command.mutable_register_();
        request->mutable_process()->set_process_id(id);
        request->mutable_process()->set_instance("first");
        request->mutable_process()->set_endpoint(std::string(id) + ":50052");
        request->mutable_process()->set_role(internal::v1::PROCESS_ROLE_KEEPER);
        internal::v1::RegisterResponse response;
        ASSERT_TRUE(response.ParseFromString(dynamic::apply(**store, command)));
        ASSERT_EQ(response.status().code(), 0);
    }
    ASSERT_TRUE((*store)->createChronicle("c").ok());
    auto story = (*store)->createStory("c", "s");
    ASSERT_TRUE(story.ok());
    auto state = (*store)->membershipRouteUpdate(story->id);
    ASSERT_TRUE(state.ok());
    ASSERT_EQ(state->route().keepers_size(), 2);
    EXPECT_EQ(state->route().keepers(0).process_id(), "keeper-a");
    EXPECT_EQ(state->route().keepers(1).process_id(), "keeper-b");
    EXPECT_EQ(state->route().epoch(), 1);
    EXPECT_TRUE((*store)->acquire(story->id, "writer").ok());
    store->reset();
    store = SqliteMetadataStore::open((dir.path() / "catalog").string(), testing::twoKeeperTopology());
    ASSERT_TRUE(store.ok());
    auto next = (*store)->createStory("c", "next");
    ASSERT_TRUE(next.ok());
    state = (*store)->membershipRouteUpdate(next->id);
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state->route().keepers_size(), 2);
}
TEST(DynamicBootstrapTest, RaftOpenNamesTheEndpointWhenItsPortIsInUse)
{
    int holder = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(holder, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(holder, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(holder, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(holder, reinterpret_cast<sockaddr*>(&address), &length), 0);
    const std::string endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    testing::TempDir dir;
    RaftConfig config{1, endpoint, {{1, endpoint, endpoint, endpoint}}};
    auto store = RaftMetadataStore::open((dir.path() / "catalog").string(), testing::twoKeeperTopology(), config);
    ::close(holder);
    ASSERT_FALSE(store.ok());
    EXPECT_EQ(store.status().code(), absl::StatusCode::kUnavailable);
    EXPECT_NE(std::string(store.status().message()).find("raft port " + endpoint + " in use"), std::string::npos)
            << store.status();
}
} // namespace chronolog::visor
