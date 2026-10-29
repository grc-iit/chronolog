#include "ChronologClientImpl.h"
#include "ChronologTimer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <client_errcode.h>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <thallium/serialization/stl/string.hpp>
#include <thread>
#include <type_traits>
#include <vector>

using namespace chronolog;
namespace
{
using Clock = std::chrono::steady_clock;
auto deadline() { return Clock::now() + std::chrono::seconds(5); }
} // namespace
TEST(ChronologTimer, AdvancesAcrossRepeatedAndBackwardReadings)
{
    std::array<uint64_t, 4> raw{100, 100, 90, 101};
    size_t i = 0;
    ChronologTimer timer([&] { return TimestampResult{CL_SUCCESS, raw[i++]}; });
    for(uint64_t want: {100, 101, 102, 103})
    {
        auto result = timer.next(deadline());
        EXPECT_EQ(CL_SUCCESS, result.status);
        EXPECT_EQ(want, result.timestamp);
    }
}
TEST(ChronologTimer, PreservesIncreasingRawReadings)
{
    std::array<uint64_t, 3> raw{100, 200, 300};
    size_t i = 0;
    ChronologTimer timer([&] { return TimestampResult{CL_SUCCESS, raw[i++]}; });
    for(uint64_t want: {100, 200, 300}) EXPECT_EQ(want, timer.next(deadline()).timestamp);
}
TEST(ChronologTimer, ZeroAndFailedReadingsDoNotAdvanceFloor)
{
    std::array<TimestampResult, 5> raw{{{0, 100}, {0, 0}, {-1, 900}, {-42, 800}, {0, 90}}};
    size_t i = 0;
    ChronologTimer timer([&] { return raw[i++]; });
    EXPECT_EQ(100, timer.next(deadline()).timestamp);
    for(int j = 0; j < 3; ++j)
    {
        auto result = timer.next(deadline());
        EXPECT_EQ(CL_ERR_CLOCK_UNAVAILABLE, result.status);
        EXPECT_EQ(0, result.timestamp);
    }
    EXPECT_EQ(101, timer.next(deadline()).timestamp);
}
TEST(ChronologTimer, ExhaustionNeverWraps)
{
    ChronologTimer timer([] { return TimestampResult{0, std::numeric_limits<uint64_t>::max()}; });
    EXPECT_EQ(std::numeric_limits<uint64_t>::max(), timer.next(deadline()).timestamp);
    for(int i = 0; i < 2; ++i)
    {
        auto result = timer.next(deadline());
        EXPECT_EQ(CL_ERR_TIMESTAMP_EXHAUSTED, result.status);
        EXPECT_EQ(0, result.timestamp);
    }
}
TEST(ChronologTimer, ExpiredCallDoesNotReadSourceOrAdvanceFloor)
{
    int reads = 0;
    ChronologTimer timer(
            [&]
            {
                ++reads;
                return TimestampResult{0, 100};
            });
    auto result = timer.next(Clock::now());
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, result.status);
    EXPECT_EQ(0, result.timestamp);
    EXPECT_EQ(0, reads);
    EXPECT_EQ(100, timer.next(deadline()).timestamp);
}
TEST(ChronologTimer, ContendingCallerExpiresWithoutEnteringSource)
{
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    std::atomic<int> reads{0};
    ChronologTimer timer(
            [&]
            {
                if(reads.fetch_add(1) == 0)
                {
                    entered.set_value();
                    released.wait();
                }
                return TimestampResult{0, 100};
            });
    auto first = std::async(std::launch::async, [&] { return timer.next(deadline()); });
    entered.get_future().wait();
    auto started = Clock::now();
    auto result = timer.next(started + std::chrono::milliseconds(25));
    int reads_before_release = reads.load();
    release.set_value();
    auto first_result = first.get();
    EXPECT_EQ(CL_SUCCESS, first_result.status);
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, result.status);
    EXPECT_EQ(0, result.timestamp);
    EXPECT_EQ(1, reads_before_release);
    EXPECT_LT(Clock::now() - started, std::chrono::seconds(1));
    EXPECT_EQ(101, timer.next(deadline()).timestamp);
}
TEST(ChronologTimer, ConcurrentAssignmentsAreUniqueAndRespectThreadOrder)
{
    ChronologTimer timer([] { return TimestampResult{0, 100}; });
    std::array<std::vector<uint64_t>, 8> outputs;
    std::vector<std::thread> workers;
    for(auto& output: outputs)
        workers.emplace_back(
                [&timer, &output]
                {
                    for(int i = 0; i < 256; ++i)
                    {
                        auto r = timer.next(deadline());
                        if(r.status == 0)
                            output.push_back(r.timestamp);
                    }
                });
    for(auto& worker: workers) worker.join();
    std::vector<uint64_t> all;
    for(auto const& output: outputs)
    {
        EXPECT_EQ(256, output.size());
        EXPECT_TRUE(std::is_sorted(output.begin(), output.end()));
        EXPECT_EQ(output.end(), std::adjacent_find(output.begin(), output.end()));
        all.insert(all.end(), output.begin(), output.end());
    }
    std::sort(all.begin(), all.end());
    ASSERT_EQ(2048, all.size());
    EXPECT_EQ(100, all.front());
    EXPECT_EQ(2147, all.back());
    EXPECT_EQ(all.end(), std::adjacent_find(all.begin(), all.end()));
}
TEST(ChronologTimer, ThrowingSourceDoesNotChangeFloor)
{
    int reads = 0;
    ChronologTimer timer(
            [&]() -> TimestampResult
            {
                if(++reads == 2)
                    throw std::runtime_error("clock");
                return {0, 100};
            });
    EXPECT_EQ(100, timer.next(deadline()).timestamp);
    auto result = timer.next(deadline());
    EXPECT_EQ(CL_ERR_CLOCK_UNAVAILABLE, result.status);
    EXPECT_EQ(0, result.timestamp);
    EXPECT_EQ(101, timer.next(deadline()).timestamp);
}
TEST(ChronologTimer, SourceReturningAfterExpiryDoesNotChangeFloor)
{
    int reads = 0;
    ChronologTimer timer(
            [&]
            {
                if(++reads == 1)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return TimestampResult{0, 100};
            });
    auto result = timer.next(Clock::now() + std::chrono::milliseconds(5));
    EXPECT_EQ(CL_ERR_DEADLINE_EXCEEDED, result.status);
    EXPECT_EQ(0, result.timestamp);
    EXPECT_EQ(100, timer.next(deadline()).timestamp);
}
TEST(ChronologTimer, ProductionSourceUsesUnixNanoseconds)
{
    auto before =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    ChronologTimer timer;
    auto result = timer.next(deadline());
    auto after =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
    EXPECT_EQ(CL_SUCCESS, result.status);
    EXPECT_GE(result.timestamp, static_cast<uint64_t>(before));
    EXPECT_LE(result.timestamp, static_cast<uint64_t>(after));
}
namespace
{
// Each test peer binds an OS-assigned port and owns its engine until all client
// calls and returned replies have settled. No daemon or shared service port.
class ClockPeers
{
public:
    thallium::engine engine{"ofi+sockets://127.0.0.1:0", THALLIUM_SERVER_MODE, true, 1};
    ClientPortalServiceConf portal;
    std::array<std::unique_ptr<thallium::engine>, 3> keeper_engines;
    std::mutex events_mutex;
    std::vector<LogEvent> events;
    std::vector<unsigned> routes;
    ClockPeers()
    {
        auto address = static_cast<std::string>(engine.self());
        portal.PORT = static_cast<uint16_t>(std::stoul(address.substr(address.rfind(':') + 1)));
        portal.PROVIDER_ID = 51;
        engine.define(
                "Connect",
                [](thallium::request const& req, uint32_t, ClientId id, uint32_t)
                { req.respond(ConnectResponseMsg(CL_SUCCESS, id)); },
                51);
        engine.define("Disconnect", [](thallium::request const& req, ClientId) { req.respond(CL_SUCCESS); }, 51);
        engine.define(
                "ReleaseStory",
                [](thallium::request const& req, ClientId, std::string const&, std::string const&)
                { req.respond(CL_SUCCESS); },
                51);
        std::vector<ServiceId> keepers;
        for(unsigned i = 0; i < 3; ++i)
        {
            auto& keeper = keeper_engines[i];
            keeper = std::make_unique<thallium::engine>("ofi+sockets://127.0.0.1:0", THALLIUM_SERVER_MODE, true, 1);
            auto address = static_cast<std::string>(keeper->self());
            auto port = static_cast<uint16_t>(std::stoul(address.substr(address.rfind(':') + 1)));
            keepers.emplace_back("ofi+sockets", "127.0.0.1", port, 52);
            keeper->define(
                    "record_event",
                    [this, i](thallium::request const& req, LogEvent const& event)
                    {
                        {
                            std::lock_guard<std::mutex> lock(events_mutex);
                            events.push_back(event);
                            routes.push_back(i);
                        }
                        req.respond(CL_SUCCESS);
                    },
                    52);
        }
        engine.define(
                "AcquireStory",
                [keepers](thallium::request const& req, ClientId, std::string const&, std::string const& story)
                { req.respond(AcquireStoryResponseMsg(CL_SUCCESS, story == "a" ? 1 : 2, keepers)); },
                51);
    }
    ~ClockPeers()
    {
        for(auto& keeper: keeper_engines) keeper->finalize();
        engine.finalize();
    }
};
} // namespace
TEST(ChronologClockOwnership, SharesFloorAcrossStoriesAndActualReconnectAndStopsClockFailuresBeforeSend)
{
    ClockPeers peers;
    peers.portal.RPC_POLICY.append_ms = 100;
    TimestampResult raw{0, 100};
    std::atomic<bool> block{false};
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto* owner = ChronologClientImpl::GetClientImplInstance(peers.portal,
                                                             WRITER_MODE,
                                                             ClientQueryServiceConf{},
                                                             [&]
                                                             {
                                                                 if(block.exchange(false))
                                                                 {
                                                                     entered.set_value();
                                                                     released.wait();
                                                                 }
                                                                 return raw;
                                                             });
    std::unique_ptr<ChronologClientImpl> cleanup(owner);
    ASSERT_EQ(CL_SUCCESS, owner->Connect());
    auto a = owner->AcquireStory("clock", "a");
    auto b = owner->AcquireStory("clock", "b");
    ASSERT_EQ(CL_SUCCESS, a.first);
    ASSERT_EQ(CL_SUCCESS, b.first);
    EXPECT_EQ(100, a.second->log_event("a1"));
    EXPECT_EQ(101, b.second->log_event("b1"));
    block = true;
    auto first = std::async(std::launch::async, [&] { return a.second->log_event("blocked source"); });
    entered.get_future().wait();
    auto started = Clock::now();
    auto expired = b.second->log_event("contending timer");
    auto elapsed = Clock::now() - started;
    release.set_value();
    EXPECT_EQ(0, first.get());
    EXPECT_EQ(0, expired);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
    EXPECT_THROW(ChronologClientImpl::GetClientImplInstance(peers.portal,
                                                            WRITER_MODE,
                                                            ClientQueryServiceConf{},
                                                            [] { return TimestampResult{0, 9999}; }),
                 std::invalid_argument);
    ASSERT_EQ(CL_SUCCESS, owner->ReleaseStory("clock", "a"));
    ASSERT_EQ(CL_SUCCESS, owner->ReleaseStory("clock", "b"));
    ASSERT_EQ(CL_SUCCESS, owner->Disconnect());
    ASSERT_EQ(CL_SUCCESS, owner->Connect());
    a = owner->AcquireStory("clock", "a");
    ASSERT_EQ(CL_SUCCESS, a.first);
    raw = {0, 90};
    EXPECT_EQ(102, a.second->log_event("after reconnect"));
    for(auto failure: std::array<TimestampResult, 3>{{{0, 0}, {-1, 800}, {-42, 900}}})
    {
        raw = failure;
        EXPECT_EQ(0, a.second->log_event("invalid"));
    }
    raw = {0, 100};
    EXPECT_EQ(103, a.second->log_event("after errors"));
    raw = {0, std::numeric_limits<uint64_t>::max()};
    EXPECT_EQ(raw.timestamp, a.second->log_event("last"));
    EXPECT_EQ(0, a.second->log_event("exhausted"));
    {
        std::lock_guard<std::mutex> lock(peers.events_mutex);
        ASSERT_EQ(5, peers.events.size());
        EXPECT_EQ((std::vector<unsigned>{1, 2, 0, 1, 0}), peers.routes);
        EXPECT_EQ(1, peers.events[0].eventIndex);
        EXPECT_EQ(2, peers.events[1].eventIndex);
        EXPECT_EQ(3, peers.events[2].eventIndex);
        EXPECT_EQ(4, peers.events[3].eventIndex);
        EXPECT_EQ(5, peers.events[4].eventIndex);
        EXPECT_EQ(1, peers.events[0].storyId);
        EXPECT_EQ(2, peers.events[1].storyId);
        EXPECT_EQ("after reconnect", peers.events[2].logRecord);
    }
    EXPECT_EQ(CL_SUCCESS, owner->ReleaseStory("clock", "a"));
    EXPECT_EQ(CL_SUCCESS, owner->Disconnect());
}
TEST(StorytellerEventIndex, StartsAtOneAndUsesUnsignedRollover)
{
    thallium::engine engine("ofi+sockets", THALLIUM_CLIENT_MODE, true, 1);
    {
        ChronologTimer timer;
        StorytellerClient client(timer, engine, 1);
        EXPECT_TRUE((std::is_same<decltype(client.get_event_index()), chrono_index>::value));
        EXPECT_EQ(1, client.get_event_index());
        EXPECT_EQ(2, client.get_event_index());
        StorytellerClient wrapping(timer,
                                   engine,
                                   1,
                                   std::make_shared<RpcPolicyContext>(RpcTimeoutPolicy{}),
                                   std::numeric_limits<chrono_index>::max() - 1);
        EXPECT_EQ(std::numeric_limits<chrono_index>::max(), wrapping.get_event_index());
        EXPECT_EQ(0, wrapping.get_event_index());
        EXPECT_EQ(1, wrapping.get_event_index());
    }
    engine.finalize();
}
