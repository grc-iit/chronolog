// Unit tests for the client's durable send path: StoryHandleImpl's
// log_event(record, options, event_time) and
// KeeperRecordingClient::send_durable_event_msg. Runs over ofi+sockets on
// 127.0.0.1 against a fake keeper that defines record_event and
// record_event_durable, and against a v3.3.0-style keeper that defines only
// record_event. The tests that kill a keeper run it in a child process (see
// KeeperProcess); the others run it in the test process.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

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
// set in the environment of a KeeperProcess; names the file its address goes to
constexpr char kKeeperAddressFileEnv[] = "CHRONOLOG_DURABLE_SEND_KEEPER_ADDRESS_FILE";
// set to 1 in the environment of a KeeperProcess whose first durable request is dropped
constexpr char kKeeperDropFirstDurableEnv[] = "CHRONOLOG_DURABLE_SEND_KEEPER_DROP_FIRST_DURABLE";

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
        // as every keeper since 3.1; the client asks it to tell a keeper
        // without record_event_durable from a dead one
        define("tail_get_sequences", &FakeKeeper::tail_get_sequences);
        if(with_durable_rpc)
        {
            durableRpc = define("record_event_durable", &FakeKeeper::record_event_durable);
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

    void tail_get_sequences(tl::request const& request, chl::StoryId const&, uint64_t)
    {
        request.respond(std::vector<chl::EventSequence>());
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
            if(!durableLog.empty())
            {
                std::ofstream(durableLog, std::ios::app) << event.time() << std::endl;
            }
        }
        if(dropAndDeregister.exchange(false))
        {
            // no answer, so the client times out; its retry finds no
            // record_event_durable here. The request is never answered: once
            // the RPC is deregistered, HG_Respond reads its freed registration.
            // Mercury 2.4.1 then cannot destroy the context that holds the
            // request, so this keeper runs only in a KeeperProcess, which is
            // killed instead of finalized.
            durableRpc.deregister();
            active--;
            return;
        }
        if(hold || holdOne.exchange(false))
        {
            waitUntilReleased();
        }
        request.respond(durableCode.load());
        active--;
    }

    // Holds the handler until the test sets released, at most 5 s. Yields the
    // ULT, so held requests do not block the handler xstreams.
    void waitUntilReleased()
    {
        auto const until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(!released && std::chrono::steady_clock::now() < until) { tl::thread::sleep(theEngine, 10); }
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
    std::atomic<bool> holdOne{false}; // hold only the next request
    std::atomic<bool> released{false};
    std::atomic<int> active{0};
    // the next record_event_durable request gets no answer and removes the RPC
    std::atomic<bool> dropAndDeregister{false};
    // when set, the time of each record_event_durable event is appended to this file
    std::string durableLog;

private:
    tl::engine& theEngine;
    tl::remote_procedure durableRpc;
    mutable std::mutex mutex;
    std::vector<chl::LogEvent> acceptedEvents;
    std::vector<chl::LogEvent> durableEvents;
    std::vector<std::pair<std::string, std::string>> durableNames;
};

uint16_t portOf(std::string const& address)
{
    return static_cast<uint16_t>(std::stoul(address.substr(address.rfind(':') + 1)));
}

// A FakeKeeper with record_event_durable in a process of its own, so a test
// can kill it as a crash would: the kernel then closes its sockets at once.
// Finalizing an engine in the test process does not reliably stand in for
// that. With Mercury 2.4.1, HG_Finalize can stop in the multi-recv cancel path
// ("Operation ID was completed") and leave the endpoint open, and a forward to
// it then times out instead of failing. A keeper that never answers a request
// runs here too, since Mercury 2.4.1 cannot destroy a context that holds one.
//
// The process is this test executable started with kKeeperAddressFileEnv set:
// main() then runs serveKeeperProcess(), which writes the keeper's address to
// that file and waits.
class KeeperProcess
{
public:
    // drop_first_durable: the first record_event_durable request gets no
    // answer and removes the RPC (FakeKeeper::dropAndDeregister)
    explicit KeeperProcess(bool drop_first_durable = false)
    {
        static int count = 0;
        theAddressFile =
                "/tmp/chronolog_durable_send_keeper_" + std::to_string(getpid()) + "_" + std::to_string(count++);
        std::remove(theAddressFile.c_str());
        std::remove(durableLog().c_str());

        std::vector<std::string> env_strings;
        for(char** e = environ; *e != nullptr; ++e) { env_strings.emplace_back(*e); }
        env_strings.push_back(std::string(kKeeperAddressFileEnv) + "=" + theAddressFile);
        if(drop_first_durable)
        {
            env_strings.push_back(std::string(kKeeperDropFirstDurableEnv) + "=1");
        }
        std::vector<char*> envp;
        for(auto& e: env_strings) { envp.push_back(e.data()); }
        envp.push_back(nullptr);

        std::string exe = "/proc/self/exe";
        char* argv[] = {exe.data(), nullptr};
        int const rc = posix_spawn(&thePid, exe.c_str(), nullptr, nullptr, argv, envp.data());
        if(rc != 0)
        {
            thePid = -1;
            return;
        }

        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while(std::chrono::steady_clock::now() < deadline)
        {
            std::ifstream in(theAddressFile);
            if(in && std::getline(in, theAddress) && !theAddress.empty())
            {
                return;
            }
            if(waitpid(thePid, nullptr, WNOHANG) == thePid)
            {
                thePid = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    ~KeeperProcess()
    {
        kill();
        std::remove(theAddressFile.c_str());
        std::remove(durableLog().c_str());
    }

    // the file the keeper appends the time of each durable event it receives to
    std::string durableLog() const { return theAddressFile + ".durable"; }

    // the times of the durable events the keeper received, in order
    std::vector<uint64_t> durableTimes() const
    {
        std::vector<uint64_t> times;
        std::ifstream in(durableLog());
        uint64_t time = 0;
        while(in >> time) { times.push_back(time); }
        return times;
    }

    KeeperProcess(KeeperProcess const&) = delete;
    KeeperProcess& operator=(KeeperProcess const&) = delete;

    // whether the keeper started and wrote its address
    bool ready() const { return thePid > 0 && !theAddress.empty(); }

    chl::ServiceId serviceId() const
    {
        return chl::ServiceId(kProtocol, "127.0.0.1", portOf(theAddress), kKeeperProvider);
    }

    // SIGKILL, as a crash; returns once the process is gone
    void kill()
    {
        if(thePid > 0)
        {
            ::kill(thePid, SIGKILL);
            waitpid(thePid, nullptr, 0);
            thePid = -1;
        }
    }

private:
    pid_t thePid = -1;
    std::string theAddressFile;
    std::string theAddress;
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
        waitForKeeperHandlers();
        if(storyteller)
        {
            storyteller->removeAcquiredStoryHandle(kChronicle, kStoryName);
            // deletes the KeeperRecordingClients
            storyteller.reset();
        }
        rawClient.reset();
        clientEngine->finalize();
        keeper.reset();
        oldKeeper.reset();
        if(keeperEngine)
        {
            keeperEngine->finalize();
        }
        oldKeeperEngine->finalize();
        keeperProcess.reset();
    }

    // A handler can still be running after its answer reached the client: it
    // decrements active after request.respond(). Destroying the FakeKeeper
    // before that is a use-after-free.
    void waitForKeeperHandlers()
    {
        if(keeper)
        {
            keeper->released = true;
            while(keeper->active > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }
    }

    static chl::ServiceId serviceIdOf(tl::engine& engine)
    {
        return chl::ServiceId(kProtocol, "127.0.0.1", portOf(engine.self()), kKeeperProvider);
    }

    chl::StoryHandle* handleOn(tl::engine& keeper_engine) { return handleOn(serviceIdOf(keeper_engine)); }

    chl::StoryHandle* handleOn(chl::ServiceId const& keeper_id)
    {
        storyteller = std::make_unique<chl::StorytellerClient>(timer, *clientEngine, kClientId);
        std::vector<chl::ServiceId> keepers{keeper_id};
        return storyteller->initializeStoryHandle(kChronicle, kStoryName, kStory, keepers, chl::ServiceId());
    }

    // a keeper in its own process, for the tests that kill it or leave a
    // request unanswered
    bool startKeeperProcess(bool drop_first_durable = false)
    {
        keeperProcess = std::make_unique<KeeperProcess>(drop_first_durable);
        return keeperProcess->ready();
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
    std::unique_ptr<KeeperProcess> keeperProcess;
};

// The process a KeeperProcess starts: a FakeKeeper with record_event_durable
// that writes its address to address_file and runs until it is killed.
[[noreturn]] void serveKeeperProcess(char const* address_file)
{
    // dies with the test that started it
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    ensureLogger();
    ensureArgobots();
    tl::engine engine(std::string(kProtocol) + "://127.0.0.1", THALLIUM_SERVER_MODE, true, 4);
    FakeKeeper keeper(engine, kKeeperProvider, true);
    keeper.durableLog = std::string(address_file) + ".durable";
    char const* drop = std::getenv(kKeeperDropFirstDurableEnv);
    keeper.dropAndDeregister = drop != nullptr && std::string(drop) == "1";
    std::string const partial = std::string(address_file) + ".partial";
    {
        std::ofstream out(partial);
        out << engine.self() << std::endl;
    }
    std::rename(partial.c_str(), address_file);
    while(true) { pause(); }
}

} // namespace

// The test executable is also the keeper process of a KeeperProcess.
int main(int argc, char** argv)
{
    if(char const* address_file = std::getenv(kKeeperAddressFileEnv))
    {
        serveKeeperProcess(address_file);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

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
    // the caller can look for the event later by its timestamp
    EXPECT_EQ(event_time, seen[0].time());
}

TEST_F(DurableSend, UnregisteredRpcIsUnavailableWithoutRetry)
{
    chl::StoryHandle* handle = handleOn(*oldKeeperEngine);
    ASSERT_NE(handle, nullptr);

    auto const start = std::chrono::steady_clock::now();
    uint64_t event_time = 55;
    EXPECT_EQ(handle->log_event("record", durable(5000, 3), event_time), chl::CL_ERR_DURABILITY_UNAVAILABLE);
    auto const elapsed = std::chrono::steady_clock::now() - start;
    // a timeout-driven retry would take at least one 5 s deadline
    EXPECT_LT(elapsed, std::chrono::milliseconds(4000));
    EXPECT_EQ(event_time, 0u);
    EXPECT_TRUE(oldKeeper->durable().empty());
    EXPECT_TRUE(oldKeeper->accepted().empty());
}

// A forward to a dead keeper fails with HG_NOENTRY, which is HG_NO_MATCH.
// That is a transport error, not a missing RPC, both once the keeper has
// answered record_event_durable and before: a client that never got an
// answer asks the keeper for an empty tail read, which a dead keeper cannot
// answer either.
TEST_F(DurableSend, DeadKeeperIsATransportErrorOnceItAnswered)
{
    ASSERT_TRUE(startKeeperProcess());
    chl::ServiceId const keeper_id = keeperProcess->serviceId();
    rawClient.reset(chl::KeeperRecordingClient::CreateKeeperRecordingClient(*clientEngine, keeper_id));
    ASSERT_NE(rawClient, nullptr);
    chl::LogEvent event(kStory, 100, kClientId, 1, "record");

    int code = 99;
    EXPECT_EQ(rawClient->send_durable_event_msg(kChronicle, kStoryName, event, 2000, code),
              chl::DurableSendOutcome::Answered);
    EXPECT_EQ(code, chl::CL_SUCCESS);

    keeperProcess->kill();
    EXPECT_EQ(rawClient->send_durable_event_msg(kChronicle, kStoryName, event, 2000, code),
              chl::DurableSendOutcome::TransportError);

    // a client that never got an answer; the old client goes first, since its
    // destructor deregisters the RPC names
    rawClient.reset();
    rawClient.reset(chl::KeeperRecordingClient::CreateKeeperRecordingClient(*clientEngine, keeper_id));
    ASSERT_NE(rawClient, nullptr);
    EXPECT_EQ(rawClient->send_durable_event_msg(kChronicle, kStoryName, event, 2000, code),
              chl::DurableSendOutcome::TransportError);
}

// The first durable write after a keeper crashed, from a client that never
// had a durable answer from it (it acquired the story, or wrote only accepted
// events): retried and reported as UNKNOWN, not as UNAVAILABLE, which would
// tell the caller that only an operator can help.
TEST_F(DurableSend, FirstDurableWriteToADeadKeeperIsRetried)
{
    ASSERT_TRUE(startKeeperProcess());
    chl::StoryHandle* handle = handleOn(keeperProcess->serviceId());
    ASSERT_NE(handle, nullptr);
    ASSERT_NE(handle->log_event("accepted"), 0u);

    keeperProcess->kill();
    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("durable", durable(300, 3), event_time), chl::CL_ERR_DURABILITY_UNKNOWN);
    EXPECT_NE(event_time, 0u);
}

// An attempt that fails at once (a dead keeper over ofi+sockets) waits out
// the rest of its timeout_ms before the next, so the attempts span
// attempts x timeout_ms, the time a restarting keeper has to come back,
// instead of a few milliseconds.
TEST_F(DurableSend, FastTransportErrorWaitsOutTheAttempt)
{
    ASSERT_TRUE(startKeeperProcess());
    chl::StoryHandle* handle = handleOn(keeperProcess->serviceId());
    ASSERT_NE(handle, nullptr);
    uint64_t event_time = 0;
    ASSERT_EQ(handle->log_event("first", durable(), event_time), chl::CL_SUCCESS);

    keeperProcess->kill();
    auto const start = std::chrono::steady_clock::now();
    EXPECT_EQ(handle->log_event("second", durable(500, 3), event_time), chl::CL_ERR_DURABILITY_UNKNOWN);
    auto const elapsed = std::chrono::steady_clock::now() - start;
    // two waits between three attempts; the last attempt does not wait
    EXPECT_GE(elapsed, std::chrono::milliseconds(950));
    EXPECT_LT(elapsed, std::chrono::milliseconds(3000));
}

// The keeper answers one durable write, then dies. The next call must not
// claim that nothing was stored: a keeper that fsynced and died before
// answering recovers the event from its WAL.
TEST_F(DurableSend, KeeperThatAnsweredThenDiedIsUnknown)
{
    ASSERT_TRUE(startKeeperProcess());
    chl::StoryHandle* handle = handleOn(keeperProcess->serviceId());
    ASSERT_NE(handle, nullptr);

    uint64_t event_time = 0;
    ASSERT_EQ(handle->log_event("first", durable(), event_time), chl::CL_SUCCESS);

    uint64_t const first_time = event_time;

    keeperProcess->kill();
    event_time = 0;
    EXPECT_EQ(handle->log_event("second", durable(1000, 3), event_time), chl::CL_ERR_DURABILITY_UNKNOWN);
    // the second event's timestamp, not 0 and not the first event's
    EXPECT_NE(event_time, 0u);
    EXPECT_NE(event_time, first_time);
}

// The first attempt reaches the keeper and times out; the second finds no
// record_event_durable there. The first attempt may have stored the event.
// The keeper runs in its own process: it never answers the first attempt.
TEST_F(DurableSend, UnregisteredRpcAfterATimeoutIsUnknown)
{
    ASSERT_TRUE(startKeeperProcess(/*drop_first_durable=*/true));
    chl::StoryHandle* handle = handleOn(keeperProcess->serviceId());
    ASSERT_NE(handle, nullptr);

    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("record", durable(300, 3), event_time), chl::CL_ERR_DURABILITY_UNKNOWN);
    auto const seen = keeperProcess->durableTimes();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(event_time, seen[0]);
}

// After a timeout, a keeper that answers "nothing stored" may be answering a
// retry of an event an earlier attempt stored.
TEST_F(DurableSend, NothingStoredAfterATimeoutIsUnknown)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    for(int code: {static_cast<int>(chl::CL_ERR_DURABILITY_UNAVAILABLE), static_cast<int>(chl::CL_ERR_DURABILITY_FULL)})
    {
        keeper->durableCode = code;
        keeper->released = false;
        // the first attempt is held past its deadline, the second answers
        keeper->holdOne = true;
        size_t const before = keeper->durable().size();
        uint64_t event_time = 0;
        EXPECT_EQ(handle->log_event("record", durable(300, 3), event_time), chl::CL_ERR_DURABILITY_UNKNOWN);
        keeper->released = true;
        while(keeper->active > 0) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        auto const seen = keeper->durable();
        ASSERT_EQ(seen.size(), before + 2);
        EXPECT_EQ(event_time, seen.back().time());
    }
}

// A keeper whose WAL failed while writing the event and could not erase it
// answers CL_ERR_DURABILITY_UNKNOWN itself.
TEST_F(DurableSend, KeeperAnswerOfUnknownReturnsTheEventTime)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);
    keeper->durableCode = chl::CL_ERR_DURABILITY_UNKNOWN;

    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("record", durable(), event_time), chl::CL_ERR_DURABILITY_UNKNOWN);
    auto const seen = keeper->durable();
    // an answer is final, so it is not retried
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(event_time, seen[0].time());
}

// The first attempt times out, and the story's only keeper leaves the handle
// before the second: the first attempt may have stored the event.
TEST_F(DurableSend, KeeperListEmptiedBetweenAttemptsIsUnknown)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);
    keeper->holdOne = true;

    uint64_t event_time = 0;
    int code = 0;
    std::thread writer([&]() { code = handle->log_event("record", durable(1000, 3), event_time); });
    // the first attempt is held in the keeper while the client waits for it
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(keeper->active == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(keeper->active.load(), 0);
    static_cast<chl::StoryHandleImpl<chl::RoundRobinKeeperChoice>*>(handle)->removeRecordingClient(
            serviceIdOf(*keeperEngine));
    writer.join();

    EXPECT_EQ(code, chl::CL_ERR_DURABILITY_UNKNOWN);
    auto const seen = keeper->durable();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(event_time, seen[0].time());
}

TEST_F(DurableSend, EmptyRecordRefusedBeforeSending)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    uint64_t event_time = 55;
    EXPECT_EQ(handle->log_event("", durable(), event_time), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(event_time, 0u);
    EXPECT_TRUE(keeper->durable().empty());
    EXPECT_TRUE(keeper->accepted().empty());
}

TEST_F(DurableSend, InvalidOptionsRefusedBeforeSending)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    uint64_t event_time = 55;
    EXPECT_EQ(handle->log_event("record", durable(0, 3), event_time), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(event_time, 0u);
    event_time = 55;
    EXPECT_EQ(handle->log_event("record", durable(1000, 0), event_time), chl::CL_ERR_INVALID_ARG);
    EXPECT_EQ(event_time, 0u);

    chl::LogEventOptions unknown_level;
    unknown_level.durability = static_cast<chl::Durability>(0);
    EXPECT_EQ(handle->log_event("record", unknown_level, event_time), chl::CL_ERR_INVALID_ARG);

    EXPECT_TRUE(keeper->durable().empty());
    EXPECT_TRUE(keeper->accepted().empty());
}

// attempts and timeout_ms are for durable writes only: an Accepted write
// with either at 0 is sent as usual.
TEST_F(DurableSend, AcceptedIgnoresAttemptsAndTimeout)
{
    chl::StoryHandle* handle = handleOn(*keeperEngine);
    ASSERT_NE(handle, nullptr);

    chl::LogEventOptions accepted;
    accepted.attempts = 0;
    accepted.timeout_ms = 0;
    uint64_t event_time = 0;
    EXPECT_EQ(handle->log_event("record", accepted, event_time), chl::CL_SUCCESS);
    EXPECT_NE(event_time, 0u);
    EXPECT_EQ(keeper->accepted().size(), 1u);
    EXPECT_TRUE(keeper->durable().empty());
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

    // margo 0.13.1 registers the provider's id of record_event_durable on the
    // first forward without a lock (margo_provider_iforward_internal): two
    // first forwards at once each allocate its margo_rpc_data, and one is
    // leaked. One call before the threads start keeps that out of this test.
    uint64_t first_time = 0;
    ASSERT_EQ(handle->log_event("first", durable(), first_time), chl::CL_SUCCESS);

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
    ASSERT_EQ(seen.size(), static_cast<size_t>(kThreads * kCallsPerThread) + 1);
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

// Two keepers' KeeperRecordingClients on one client engine share their RPC IDs
// (#743). Deleting one used to deregister them, and the other client's
// record_event and record_event_durable then failed with HG_NOENTRY without
// leaving the process.
class KeeperClientLifetime: public DurableSend
{
};

TEST_F(KeeperClientLifetime, DeletingOneClientLeavesTheOtherWorking)
{
    auto* leaving =
            chl::KeeperRecordingClient::CreateKeeperRecordingClient(*clientEngine, serviceIdOf(*oldKeeperEngine));
    rawClient.reset(chl::KeeperRecordingClient::CreateKeeperRecordingClient(*clientEngine, serviceIdOf(*keeperEngine)));
    ASSERT_NE(leaving, nullptr);
    ASSERT_NE(rawClient, nullptr);
    chl::LogEvent event(kStory, 100, kClientId, 1, "record");
    ASSERT_EQ(leaving->send_event_msg(event), chl::CL_SUCCESS);

    delete leaving;

    EXPECT_EQ(rawClient->send_event_msg(event), chl::CL_SUCCESS);
    EXPECT_EQ(keeper->accepted().size(), 1u);
    int code = 99;
    EXPECT_EQ(rawClient->send_durable_event_msg(kChronicle, kStoryName, event, 2000, code),
              chl::DurableSendOutcome::Answered);
    EXPECT_EQ(code, chl::CL_SUCCESS);
}
