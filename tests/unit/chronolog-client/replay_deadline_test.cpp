#include "ClientQueryService.h"
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <thread>
using namespace chronolog;
using namespace std::chrono_literals;
namespace
{
PlaybackQuery query(std::chrono::steady_clock::time_point deadline, Client::EventCallback callback)
{
    return PlaybackQuery(1, deadline, 19, "chron", "story", 100, 200, std::move(callback));
}
} // namespace
TEST(ReplayCallbacks, ExpiredAdmissionNeverCallsUserCode)
{
    unsigned calls = 0;
    auto state = query(std::chrono::steady_clock::now() - 1ms, [&](Event const&) { ++calls; });
    EXPECT_FALSE(state.beginCallback());
    state.closeCallbacks();
    EXPECT_FALSE(state.callback);
    EXPECT_EQ(0u, calls);
}
TEST(ReplayCallbacks, CloseWaitsForActiveCallbackThenReleasesItsCapture)
{
    auto captured = std::make_shared<int>(1);
    std::weak_ptr<int> observed = captured;
    auto state = query(std::chrono::steady_clock::now() + 5s, [captured = std::move(captured)](Event const&) {});
    ASSERT_TRUE(state.beginCallback());
    auto closing = std::async(std::launch::async, [&] { state.closeCallbacks(); });
    while(!(state.callbackState.load() & PlaybackQuery::CallbackClosed)) std::this_thread::yield();
    EXPECT_FALSE(state.beginCallback());
    EXPECT_EQ(std::future_status::timeout, closing.wait_for(10ms));
    EXPECT_FALSE(observed.expired());
    state.endCallback();
    closing.get();
    EXPECT_TRUE(observed.expired());
    EXPECT_FALSE(state.callback);
    EXPECT_FALSE(state.beginCallback());
}
