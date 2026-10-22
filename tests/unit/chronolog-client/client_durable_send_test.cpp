// Unit tests for the client's durable send path: StoryHandleImpl's
// log_event(record, options, event_time) and
// KeeperRecordingClient::send_durable_event_msg. Runs over ofi+sockets on
// 127.0.0.1 against a fake keeper that defines record_event and
// record_event_durable, and against a v3.3.0-style keeper that defines only
// record_event.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <abt.h>
#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <chrono_monitor.h>
#include <chronolog_client.h>
#include <chronolog_types.h>
#include <client_errcode.h>
#include <ServiceId.h>

#include "KeeperRecordingClient.h"
#include "StorytellerClient.h"

namespace chl = chronolog;
namespace tl = thallium;

namespace
{
constexpr char kProtocol[] = "ofi+sockets";
constexpr uint16_t kKeeperProvider = 31;
constexpr chl::StoryId kStory = 7;
constexpr chl::ClientId kClientId = 0x1234abcd5678ull;
constexpr char kChronicle[] = "chron";
constexpr char kStoryName[] = "story";

void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "client_durable_send_test_logger");
        done = true;
    }
}

// Initialized once and never finalized, so margo leaves Argobots alone at
// engine finalize.
void ensureArgobots()
{
    static bool done = false;
    if(!done)
    {
        ABT_init(0, nullptr);
        done = true;
    }
}

chl::EventSequence identityOf(chl::LogEvent const& event)
{
    return chl::EventSequence(event.time(), event.getClientId(), event.index());
}

// A keeper with both write RPCs. Each records what it received and answers
// with the configured code; record_event_durable can instead hold every
// request past the client's deadline.
class FakeKeeper: public tl::provider<FakeKeeper>
{
public:
    FakeKeeper(tl::engine& engine, uint16_t provider_id, bool with_durable_rpc)
        : tl::provider<FakeKeeper>(engine, provider_id)
        , theEngine(engine)
    {
        define("record_event", &FakeKeeper::record_event);
        if(with_durable_rpc)
        {
            define("record_event_durable", &FakeKeeper::record_event_durable);
        }
    }

    void record_event(tl::request const& request, chl::LogEvent const& event)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            acceptedEvents.push_back(event);
        }
        request.respond(acceptedCode.load());
    }

    void record_event_durable(tl::request const& request,
                              chl::ChronicleName const& chronicle,
                              chl::StoryName const& story,
                              chl::LogEvent const& event)
    {
        active++;
        {
            std::lock_guard<std::mutex> lock(mutex);
            durableEvents.push_back(event);
            durableNames.emplace_back(chronicle, story);
        }
        if(hold)
        {
            // yields the ULT, so held requests do not block the handler xstreams
            auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while(!released && std::chrono::steady_clock::now() < until) { tl::thread::sleep(theEngine, 10); }
        }
        request.respond(durableCode.load());
        active--;
    }

    std::vector<chl::LogEvent> durable() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return durableEvents;
    }

    std::vector<chl::LogEvent> accepted() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return acceptedEvents;
    }

    std::vector<std::pair<std::string, std::string>> names() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return durableNames;
    }

    std::atomic<int> acceptedCode{chl::CL_SUCCESS};
    std::atomic<int> durableCode{chl::CL_SUCCESS};
    std::atomic<bool> hold{false};
    std::atomic<bool> released{false};
    std::atomic<int> active{0};

private:
    tl::engine& theEngine;
    mutable std::mutex mutex;
    std::vector<chl::LogEvent> acceptedEvents;
    std::vector<chl::LogEvent> durableEvents;
    std::vector<std::pair<std::string, std::string>> durableNames;
};

class DurableSend: public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureLogger();
        ensureArgobots();
        keeperEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 4);
        oldKeeperEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 1);
        clientEngine =
                std::make_unique<tl::engine>(std::string(kProtocol) + "://127.0.0.1", THALLIUM_CLIENT_MODE, true, 1);

        keeper = std::make_unique<FakeKeeper>(*keeperEngine, kKeeperProvider, true);
        oldKeeper = std::make_unique<FakeKeeper>(*oldKeeperEngine, kKeeperProvider, false);
    }

    void TearDown() override
    {
        // let held requests answer while the client engine is still up
        keeper->released = true;
        while(keeper->active > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        if(storyteller)
        {
            storyteller->removeAcquiredStoryHandle(kChronicle, kStoryName);
            // deletes the KeeperRecordingClients, which deregister their RPCs
            storyteller.reset();
        }
        rawClient.reset();
        clientEngine->finalize();
        keeper.reset();
        oldKeeper.reset();
        keeperEngine->finalize();
        oldKeeperEngine->finalize();
    }

    static chl::ServiceId serviceIdOf(tl::engine& engine)
    {
        std::string const self = engine.self();
        auto const port = static_cast<uint16_t>(std::stoul(self.substr(self.rfind(':') + 1)));
        return chl::ServiceId(kProtocol, "127.0.0.1", port, kKeeperProvider);
    }

    chl::StoryHandle* handleOn(tl::engine& keeper_engine)
    {
        storyteller = std::make_unique<chl::StorytellerClient>(timer, *clientEngine, kClientId);
        std::vector<chl::ServiceId> keepers{serviceIdOf(keeper_engine)};
        return storyteller->initializeStoryHandle(kChronicle, kStoryName, kStory, keepers, chl::ServiceId());
    }

    static chl::LogEventOptions durable(uint32_t timeout_ms = 10000, uint32_t attempts = 3)
    {
        chl::LogEventOptions options;
        options.durability = chl::Durability::Durable;
        options.timeout_ms = timeout_ms;
        options.attempts = attempts;
        return options;
    }

    chl::ChronologTimer timer;
    std::unique_ptr<tl::engine> keeperEngine;
    std::unique_ptr<tl::engine> oldKeeperEngine;
    std::unique_ptr<tl::engine> clientEngine;
    std::unique_ptr<FakeKeeper> keeper;
    std::unique_ptr<FakeKeeper> oldKeeper;
    std::unique_ptr<chl::StorytellerClient> storyteller;
    std::unique_ptr<chl::KeeperRecordingClient> rawClient;
};

} // namespace

// What thallium raises when the keeper does not define the RPC: a
// margo_exception carrying HG_NO_MATCH, not a timeout.
TEST_F(DurableSend, MissingRpcRaisesNoMatch)
{
    tl::remote_procedure rpc = clientEngine->define("record_event_durable");
    std::string address;
    serviceIdOf(*oldKeeperEngine).get_service_as_string(address);
    tl::provider_handle ph(clientEngine->lookup(address), kKeeperProvider);

    chl::LogEvent event(kStory, 100, kClientId, 1, "record");
    bool raised = false;
    try
    {
        int code = rpc.on(ph).timed(std::chrono::milliseconds(5000),
                                    chl::ChronicleName(kChronicle),
                                    chl::StoryName(kStoryName),
                                    event);
        ADD_FAILURE() << "the call answered " << code;
    }
    catch(tl::margo_exception const& ex)
    {
        raised = true;
        EXPECT_EQ(ex.error(), HG_NO_MATCH) << ex.what();
    }
    catch(tl::timeout const&)
    {
        ADD_FAILURE() << "the call timed out instead of failing with HG_NO_MATCH";
    }
    EXPECT_TRUE(raised);
    rpc.deregister();
}

TEST_F(DurableSend, SendOutcomes)
{
    rawClient.reset(chl::KeeperRecordingClient::CreateKeeperRecordingClient(*clientEngine, serviceIdOf(*keeperEngine)));
    ASSERT_NE(rawClient, nullptr);
    chl::LogEvent event(kStory, 100, kClientId, 1, "record");

    int code = 99;
    keeper->durableCode = chl::CL_ERR_DURABILITY_FULL;
    EXPECT_EQ(rawClient->send_durable_event_msg(kChronicle, kStoryName, event, 2000, code),
              chl::DurableSendOutcome::Answered);
    EXPECT_EQ(code, chl::CL_ERR_DURABILITY_FULL);

    keeper->hold = true;
    EXPECT_EQ(rawClient->send_durable_event_msg(kChronicle, kStoryName, event, 200, code),
              chl::DurableSendOutcome::TimedOut);
}

TEST_F(DurableSend, DurableSuccessSetsEventTime)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("durable record", durable(), event_time), chl::CL_SUCCESS);
    EXPECT_NE(event_time, 0u);

    auto const seen = keeper->durable();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].time(), event_time);
    EXPECT_EQ(seen[0].getClientId(), kClientId);
    EXPECT_EQ(seen[0].getStoryId(), kStory);
    EXPECT_EQ(seen[0].getRecord(), "durable record");
    auto const names = keeper->names();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0].first, kChronicle);
    EXPECT_EQ(names[0].second, kStoryName);
    EXPECT_TRUE(keeper->accepted().empty());
}

TEST_F(DurableSend, KeeperCodesPassThrough)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    for(int code: {static_cast<int>(chl::CL_ERR_DURABILITY_UNAVAILABLE), static_cast<int>(chl::CL_ERR_DURABILITY_FULL)})
    {
        keeper->durableCode = code;
        uint64_t event_time = 55;
        EXPECT_EQ(handle->log_event("record", durable(), event_time), code);
        EXPECT_EQ(event_time, 0u);
    }
    // an answer is final: neither code is retried
    EXPECT_EQ(keeper->durable().size(), 2u);
}

TEST_F(DurableSend, TimeoutRetriesTheIdenticalEvent)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);
    keeper->hold = true;

    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("record", durable(200, 3), event_time), chl::CL_ERR_DURABILITY_UNKNOWN);

    // the attempts are sequential, but the last one may still be in flight
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(keeper->durable().size() < 3 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto const seen = keeper->durable();
    ASSERT_EQ(seen.size(), 3u);
    for(auto const& event: seen)
    {
        EXPECT_EQ(identityOf(event), identityOf(seen[0]));
        EXPECT_EQ(event.getRecord(), "record");
    }
}

TEST_F(DurableSend, UnregisteredRpcIsUnavailableWithoutRetry)
{
    chl::StoryHandle* handle = handleOn(*oldKeeperEngine);
    ASSERT_NE(handle, nullptr);

    auto const start = std::chrono::steady_clock::now();
    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("record", durable(5000, 3), event_time), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    auto const elapsed = std::chrono::steady_clock::now() - start;
    // a timeout-driven retry would take at least one 5 s deadline
    EXPECT_LT(elapsed, std::chrono::milliseconds(4000));
    EXPECT_EQ(event_time, 0u);
    EXPECT_TRUE(oldKeeper->durable().empty());
    EXPECT_TRUE(oldKeeper->accepted().empty());
}

TEST_F(DurableSend, EmptyRecordRefusedBeforeSending)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("", durable(), event_time), chl::CL_ERR_INVALID_ARG);
    EXPECT_TRUE(keeper->durable().empty());
    EXPECT_TRUE(keeper->accepted().empty());
}

TEST_F(DurableSend, InvalidOptionsRefusedBeforeSending)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("record", durable(0, 3), event_time), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(handle->log_event("record", durable(1000, 0), event_time), chl::CL_ERR_INVALID_ARG);

    chl::LogEventOptions accepted;
    accepted.attempts = 0;
    EXPECT_EQ(handle->log_event("record", accepted, event_time), chl::CL_ERR_INVALID_ARG);
    accepted = chl::LogEventOptions{};
    accepted.timeout_ms = 0;
    EXPECT_EQ(handle->log_event("record", accepted, event_time), chl::CL_ERR_INVALID_ARG);

    chl::LogEventOptions unknown_level;
    unknown_level.durability = static_cast<chl::Durability>(0);
    EXPECT_EQ(handle->log_event("record", unknown_level, event_time), chl::CL_ERR_INVALID_ARG);

    EXPECT_TRUE(keeper->durable().empty());
    EXPECT_TRUE(keeper->accepted().empty());
}

TEST_F(DurableSend, AcceptedReturnsKeeperStatus)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("first", chl::LogEventOptions{}, event_time), chl::CL_SUCCESS);
    EXPECT_NE(event_time, 0u);

    // a code log_event(record) would collapse to 0, such as the
    // acceptance-window work's refusals
    keeper->acceptedCode = -15;
    uint64_t refused_time = 77;
    EXPECT_EQ(handle->log_event("second", chl::LogEventOptions{}, refused_time), -15);
    EXPECT_EQ(refused_time, 0u);

    auto const seen = keeper->accepted();
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].time(), event_time);
    EXPECT_EQ(seen[1].getRecord(), "second");
    EXPECT_TRUE(keeper->durable().empty());
}

TEST_F(DurableSend, ConcurrentCallsOnOneHandle)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    constexpr int kThreads = 8;
    constexpr int kCallsPerThread = 100;
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for(int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
                [&, t]()
                {
                    for(int i = 0; i < kCallsPerThread; ++i)
                    {
                        uint64_t event_time = 0;
                        std::string const record = std::to_string(t) + ":" + std::to_string(i);
                        if(handle->log_event(record, durable(), event_time) == chl::CL_SUCCESS && event_time != 0)
                        {
                            successes++;
                        }
                        else
                        {
                            failures++;
                        }
                    }
                });
    }
    for(auto& thread: threads) { thread.join(); }

    EXPECT_EQ(successes.load(), kThreads * kCallsPerThread);
    EXPECT_EQ(failures.load(), 0);

    auto const seen = keeper->durable();
    ASSERT_EQ(seen.size(), static_cast<size_t>(kThreads * kCallsPerThread));
    std::set<chl::EventSequence> identities;
    std::set<std::string> records;
    for(auto const& event: seen)
    {
        identities.insert(identityOf(event));
        records.insert(event.getRecord());
    }
    EXPECT_EQ(identities.size(), seen.size());
    EXPECT_EQ(records.size(), seen.size());
}
