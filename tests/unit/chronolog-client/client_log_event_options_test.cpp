#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <chronolog_client.h>

using chronolog::Durability;
using chronolog::LogEventOptions;

namespace
{

// Overrides only log_event(record), as a handle written before the options
// overload existed would. The using-declaration keeps the inherited overload
// visible on the derived type.
class FixedTimeStoryHandle: public chronolog::StoryHandle
{
public:
    explicit FixedTimeStoryHandle(uint64_t time)
        : returnedTime(time)
    {}

    using chronolog::StoryHandle::log_event;

    uint64_t log_event(std::string const& record) override
    {
        ++calls;
        lastRecord = record;
        return returnedTime;
    }

    uint64_t returnedTime;
    int calls = 0;
    std::string lastRecord;
};

} // namespace

TEST(LogEventOptions, Defaults)
{
    LogEventOptions options;
    EXPECT_EQ(options.durability, Durability::Accepted);
    EXPECT_EQ(options.timeout_ms, 10000u);
    EXPECT_EQ(options.attempts, 3u);

    EXPECT_EQ(static_cast<int>(Durability::Accepted), 1);
    EXPECT_EQ(static_cast<int>(Durability::Durable), 2);
    EXPECT_EQ(sizeof(Durability), 1u);
}

TEST(LogEventOptions, DefaultAcceptedReturnsSuccessAndEventTime)
{
    FixedTimeStoryHandle handle(1234567890123ull);
    uint64_t event_time = 0;
    EXPECT_EQ(handle.log_event("record", LogEventOptions{}, event_time), chronolog::CL_SUCCESS);
    EXPECT_EQ(event_time, 1234567890123ull);
    EXPECT_EQ(handle.calls, 1);
    EXPECT_EQ(handle.lastRecord, "record");
}

TEST(LogEventOptions, DefaultAcceptedReturnsUnknownOnZeroTime)
{
    FixedTimeStoryHandle handle(0);
    uint64_t event_time = 77;
    chronolog::StoryHandle& base = handle;
    EXPECT_EQ(base.log_event("record", LogEventOptions{}, event_time), chronolog::CL_ERR_UNKNOWN);
    EXPECT_EQ(event_time, 0u);
    EXPECT_EQ(handle.calls, 1);
}

TEST(LogEventOptions, DefaultDurableIsUnavailableAndSendsNothing)
{
    FixedTimeStoryHandle handle(42);
    LogEventOptions options;
    options.durability = Durability::Durable;
    uint64_t event_time = 0;
    EXPECT_EQ(handle.log_event("record", options, event_time), chronolog::CL_ERR_DURABILITY_UNAVAILABLE);
    EXPECT_EQ(handle.calls, 0);
}
