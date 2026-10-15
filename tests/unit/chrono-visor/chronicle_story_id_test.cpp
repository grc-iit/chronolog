#include <gtest/gtest.h>

#include <Chronicle.h>

namespace
{
uint64_t addStoryTo(Chronicle& chronicle, std::string const& chronicle_name, std::string const& story_name)
{
    chronicle.setName(chronicle_name);
    auto const [status, story] = chronicle.addStory(story_name);
    EXPECT_EQ(status, chronolog::CL_SUCCESS);
    return (story == nullptr) ? 0 : story->getSid();
}
} // namespace

// The chronicle and story names used to be hashed as one concatenated string,
// so ("ab", "c") and ("a", "bc") got the same story id and shared a pipeline,
// a watermark and receipts on every server.
TEST(ChronicleStoryId, NamesThatConcatenateAlikeGetDifferentIds)
{
    Chronicle first;
    Chronicle second;
    uint64_t const ab_c = addStoryTo(first, "ab", "c");
    uint64_t const a_bc = addStoryTo(second, "a", "bc");

    EXPECT_NE(ab_c, 0u);
    EXPECT_NE(a_bc, 0u);
    EXPECT_NE(ab_c, a_bc);

    // a Chronicle frees its stories only on removal
    EXPECT_EQ(first.removeStory("ab", "c"), chronolog::CL_SUCCESS);
    EXPECT_EQ(second.removeStory("a", "bc"), chronolog::CL_SUCCESS);
}

TEST(ChronicleStoryId, LookupAndRemovalUseTheSameId)
{
    Chronicle chronicle;
    uint64_t const sid = addStoryTo(chronicle, "ab", "c");

    EXPECT_TRUE(chronicle.hasStory("c"));
    EXPECT_EQ(chronicle.getStoryId("c"), sid);
    EXPECT_EQ(Chronicle::storyIdOf("ab", "c"), sid);
    EXPECT_EQ(chronicle.removeStory("ab", "c"), chronolog::CL_SUCCESS);
    EXPECT_FALSE(chronicle.hasStory("c"));
}
