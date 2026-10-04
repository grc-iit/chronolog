#include <gtest/gtest.h>
#include "common/predicate/Predicate.h"

namespace chronolog
{
namespace
{

Event event()
{
    Event e;
    e.id = {1, 2, 1, 3};
    e.envelope.kind = "note";
    e.envelope.actor = "agent-a";
    e.envelope.attributes = {{"k", "v"}, {"x", "y"}};
    e.envelope.links = {{"cites", {1, 7, 1, 9}, std::nullopt}, {"replies", {1, 8, 1, 1}, Hlc{5, 0}}};
    return e;
}

TEST(EventPredicate, UnsetMatchesEveryEvent)
{
    EventPredicate p;
    EXPECT_TRUE(p.empty());
    EXPECT_TRUE(p.validate().ok());
    EXPECT_TRUE(p.matches(event()));
    EXPECT_TRUE(p.matches(Event{}));
}

TEST(EventPredicate, SetsMatchAnyMemberAndTermsAreConjoined)
{
    EventPredicate p;
    p.kinds = {"other", "note"};
    EXPECT_TRUE(p.matches(event()));
    p.actors = {"agent-b"};
    EXPECT_FALSE(p.matches(event()));
    p.actors.push_back("agent-a");
    EXPECT_TRUE(p.matches(event()));
}

TEST(EventPredicate, AttributeTermsNeedKeyAndValue)
{
    EventPredicate p;
    p.attributes = {{"k", "v"}};
    EXPECT_TRUE(p.matches(event()));
    p.attributes.push_back({"x", "z"});
    EXPECT_FALSE(p.matches(event()));
}

TEST(EventPredicate, LinkTermMatchesTargetAndOptionalTypeAndIgnoresHlc)
{
    EventPredicate p;
    p.links = {{"", {1, 8, 1, 1}}};
    EXPECT_TRUE(p.matches(event()));
    p.links[0].type = "replies";
    EXPECT_TRUE(p.matches(event()));
    p.links[0].type = "cites";
    EXPECT_FALSE(p.matches(event()));
    p.links[0].target = {1, 7, 1, 9};
    EXPECT_TRUE(p.matches(event()));
    p.links[0].target = {1, 7, 1, 10};
    EXPECT_FALSE(p.matches(event()));
}

TEST(EventPredicate, EventIdTermMatchesAnyMember)
{
    EventPredicate p;
    p.event_ids = {{1, 2, 1, 4}};
    EXPECT_FALSE(p.matches(event()));
    p.event_ids.push_back({1, 2, 1, 3});
    EXPECT_TRUE(p.matches(event()));
}

TEST(EventPredicate, MalformedTermsAreInvalidArgument)
{
    EventPredicate p;
    p.attributes = {{"", "v"}};
    EXPECT_EQ(p.validate().code(), absl::StatusCode::kInvalidArgument);
    p = {};
    p.links = {{"cites", {}}};
    EXPECT_EQ(p.validate().code(), absl::StatusCode::kInvalidArgument);
    p = {};
    p.links = {{"cites", {1, 7, 1, 0}}};
    EXPECT_EQ(p.validate().code(), absl::StatusCode::kInvalidArgument);
    p = {};
    p.event_ids = {{1, 0, 1, 1}};
    EXPECT_EQ(p.validate().code(), absl::StatusCode::kInvalidArgument);
}

TEST(EventPredicate, MoreThan256ValuesAreInvalidArgument)
{
    EventPredicate p;
    p.kinds.assign(EventPredicate::kMaxValues, "k");
    EXPECT_TRUE(p.validate().ok());
    p.actors.push_back("a");
    EXPECT_EQ(p.validate().code(), absl::StatusCode::kInvalidArgument);
}

} // namespace
} // namespace chronolog
