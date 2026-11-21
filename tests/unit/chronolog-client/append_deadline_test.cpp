#include <chronolog_client.h>
#include <gtest/gtest.h>
using namespace chronolog;
namespace
{
// This application subclass implements only the original pure virtual method.
class ExternalHandle: public StoryHandle
{
public:
    uint64_t value = 0;
    uint64_t log_event(std::string const&) override { return value; }
};
} // namespace
TEST(AppendCompatibility, LegacySubclassGetsAcknowledgementAndUncertaintyFallback)
{
    ExternalHandle handle;
    handle.value = 123;
    AppendResult ack = handle.append_event("ok");
    EXPECT_EQ(0, ack.status);
    EXPECT_EQ(AppendOutcome::Acknowledged, ack.outcome);
    EXPECT_EQ(123u, ack.timestamp);
    EXPECT_EQ(0u, ack.event_index);
    handle.value = 0;
    AppendResult unknown = handle.append_event("unknown");
    EXPECT_EQ(-23, unknown.status);
    EXPECT_EQ(AppendOutcome::Unconfirmed, unknown.outcome);
    EXPECT_EQ(0u, unknown.timestamp);
    EXPECT_EQ(0u, unknown.event_index);
}
