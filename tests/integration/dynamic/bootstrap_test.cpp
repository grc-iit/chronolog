#include <gtest/gtest.h>
#include "TestSupport.h"
#include "dynamic/MembershipState.h"

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
} // namespace chronolog::visor
