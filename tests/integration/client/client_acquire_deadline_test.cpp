#include "../../unit/chrono-visor/admin_test_fixture.h"
#include <AcquireStoryResponseMsg.h>
#include <chronolog_client.h>
#include <chrono>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>
#include <functional>

extern char** environ;
struct NativeAcquire final: rpc::OwnedRpcWork<std::shared_ptr<AcquireStoryResponseMsg>>
{
    std::shared_ptr<tl::engine> engine;
    std::string address;
    tl::remote_procedure remote;
    std::string story = "story";
    rpc::RpcCallState native;
    rpc::RpcResult<std::shared_ptr<AcquireStoryResponseMsg>> run(rpc::OperationContext& context) override
    {
        native.bind(*engine, remote, address, 40);
        auto cap = context.deadline().cappedAttempt(120ms);
        if(!cap || !context.tryBeginDispatch())
            return {CL_ERR_DEADLINE_EXCEEDED, false, std::nullopt};
        auto value = native.callable()
                             .timed(*cap, ClientId{100}, std::string("chronicle"), story)
                             .as<AcquireStoryResponseMsg>();
        auto reply = std::make_shared<AcquireStoryResponseMsg>(value);
        native.clearKnown();
        return {value.getErrorCode(), true, std::move(reply)};
    }
};
class ClientAcquireDeadline: public AdminDeadline
{
protected:
    std::shared_ptr<tl::engine> clientEngine;
    tl::remote_procedure acquireRpc;
    // Match the accepted native B01 owner: a dedicated RPC stream also keeps
    // native timed-call callbacks off the test thread that sleeps on std clocks.
    void SetUp() override
    {
        AdminDeadline::SetUp();
        clientEngine = std::make_shared<tl::engine>(protocol, THALLIUM_CLIENT_MODE, true, 1);
        acquireRpc = clientEngine->define("AcquireStory");
    }
    rpc::RpcResult<std::shared_ptr<AcquireStoryResponseMsg>>
    acquire(VisorClientPortal& portal, rpc::RpcRuntime& caller, std::chrono::milliseconds budget)
    {
        auto deadline = rpc::RpcDeadline::after(budget);
        auto token = caller.reserve(deadline);
        auto work = std::make_shared<NativeAcquire>();
        work->engine = clientEngine;
        work->remote = acquireRpc;
        work->address = std::string(portal.core()->engine->self());
        return caller.invoke<std::shared_ptr<AcquireStoryResponseMsg>>(std::move(token), std::move(work));
    }
};
TEST_P(ClientAcquireDeadline, NativePortalSerializesOnlyConfirmedResponsiveRoster)
{
    VisorClientPortal portal;
    attach(portal, true);
    rpc::RpcRuntime caller;
    auto begin = std::chrono::steady_clock::now();
    auto result = acquire(portal, caller, 220ms);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 260ms);
    ASSERT_TRUE(result.reply);
    auto& response = **result.reply;
    EXPECT_EQ(response.getErrorCode(), CL_SUCCESS);
    ASSERT_EQ(response.getKeepers().size(), 1u);
    EXPECT_EQ(to_string(response.getKeepers()[0]), to_string(keeper(3).getRecordingServiceId()));
    EXPECT_EQ(to_string(response.getPlayer()), to_string(service(5)));
    EXPECT_EQ(caller.drain(rpc::RpcDeadline::after(200ms)), CL_SUCCESS);
}
TEST_P(ClientAcquireDeadline, LostClientReplyLeavesDuplicateAcquireErrorWithEmptyRoster)
{
    delays[0] = 70;
    delays[1] = delays[2] = 0;
    VisorClientPortal portal;
    attach(portal, true);
    rpc::RpcRuntime caller;
    auto result = acquire(portal, caller, 25ms);
    EXPECT_FALSE(result.reply);
    EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_TRUE(result.may_have_dispatched);
    std::this_thread::sleep_for(150ms);
    auto duplicate = portal.AcquireStory(100, "chronicle", "story");
    EXPECT_EQ(duplicate.getErrorCode(), CL_ERR_ACQUIRED);
    EXPECT_TRUE(duplicate.getKeepers().empty());
    EXPECT_FALSE(result.reply);
}
TEST_P(ClientAcquireDeadline, SaturationRejectsBeforeOwningRequestAndCallerWaitIsBounded)
{
    VisorClientPortal portal;
    attach(portal, true);
    rpc::RpcRuntime caller;
    auto owner = registry->adminOwner();
    auto deadline = rpc::RpcDeadline::after(500ms);
    auto tokens = occupy(32, deadline);
    ASSERT_EQ(tokens.size(), 32u);
    auto before = portal.core()->incomingRequests.load();
    auto begin = std::chrono::steady_clock::now();
    auto result = acquire(portal, caller, 120ms);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 220ms);
    EXPECT_FALSE(result.reply);
    EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_EQ(portal.core()->incomingRequests.load(), before);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);
    tokens.clear();
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
}
INSTANTIATE_TEST_SUITE_P(Native, ClientAcquireDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));

namespace
{
struct PublicProbeStartupCalls
{
    std::function<int(int*, int)> pipe = ::pipe2;
    decltype(&::posix_spawn_file_actions_init) initializeActions = ::posix_spawn_file_actions_init;
    decltype(&::posix_spawn) spawn = ::posix_spawn;
};

class PublicClientProbe
{
public:
    PublicClientProbe() = default;
    PublicClientProbe(PublicClientProbe const&) = delete;
    PublicClientProbe& operator=(PublicClientProbe const&) = delete;
    ~PublicClientProbe() { cleanup(); }

    bool start(std::string const& protocol, uint16_t port, PublicProbeStartupCalls const& calls = {})
    {
        int toChild[2]{-1, -1};
        int fromChild[2]{-1, -1};
        auto closePipes = [&]
        {
            for(int descriptor: toChild)
                if(descriptor >= 0)
                    ::close(descriptor);
            for(int descriptor: fromChild)
                if(descriptor >= 0)
                    ::close(descriptor);
        };
        if(calls.pipe(toChild, O_CLOEXEC) != 0 || calls.pipe(fromChild, O_CLOEXEC) != 0)
        {
            closePipes();
            return false;
        }

        posix_spawn_file_actions_t actions;
        if(calls.initializeActions(&actions) != 0)
        {
            closePipes();
            return false;
        }
        bool actionsOk = posix_spawn_file_actions_adddup2(&actions, toChild[0], 100) == 0 &&
                         posix_spawn_file_actions_adddup2(&actions, fromChild[1], 101) == 0 &&
                         posix_spawn_file_actions_addclose(&actions, toChild[0]) == 0 &&
                         posix_spawn_file_actions_addclose(&actions, toChild[1]) == 0 &&
                         posix_spawn_file_actions_addclose(&actions, fromChild[0]) == 0 &&
                         posix_spawn_file_actions_addclose(&actions, fromChild[1]) == 0;
        std::string readFd = "100";
        std::string writeFd = "101";
        std::string portText = std::to_string(port);
        std::vector<char*> args{const_cast<char*>(CHRONOLOG_PUBLIC_PROBE_PATH),
                                readFd.data(),
                                writeFd.data(),
                                const_cast<char*>(protocol.c_str()),
                                const_cast<char*>("127.0.0.1"),
                                portText.data(),
                                const_cast<char*>("40"),
                                nullptr};
        int spawnStatus =
                actionsOk ? calls.spawn(&pid_, CHRONOLOG_PUBLIC_PROBE_PATH, &actions, nullptr, args.data(), environ)
                          : EINVAL;
        posix_spawn_file_actions_destroy(&actions);
        if(spawnStatus != 0)
        {
            pid_ = -1;
            closePipes();
            return false;
        }
        ::close(toChild[0]);
        ::close(fromChild[1]);
        input_ = toChild[1];
        output_ = fromChild[0];
        std::string ready;
        if(!readLine(ready, std::chrono::seconds(10)))
        {
            startupLine_ = "child exited or timed out before READY";
            return false;
        }
        startupLine_ = ready;
        std::istringstream parsed(ready);
        std::string marker;
        parsed >> marker >> clientId_;
        return marker == "READY" && clientId_ != 0;
    }

    ClientId clientId() const { return clientId_; }
    std::string const& startupLine() const { return startupLine_; }

    bool send(std::string const& message)
    {
        std::string bytes = message + '\n';
        std::size_t sent = 0;
        while(sent < bytes.size())
        {
            auto count = ::write(input_, bytes.data() + sent, bytes.size() - sent);
            if(count < 0 && errno == EINTR)
                continue;
            if(count <= 0)
                return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool readLine(std::string& line, std::chrono::milliseconds timeout)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        for(;;)
        {
            auto newline = buffered_.find('\n');
            if(newline != std::string::npos)
            {
                line = buffered_.substr(0, newline);
                buffered_.erase(0, newline + 1);
                return true;
            }
            auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if(remaining <= std::chrono::milliseconds::zero())
                return false;
            pollfd descriptor{output_, POLLIN, 0};
            int ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
            if(ready < 0 && errno == EINTR)
                continue;
            if(ready <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))
                return false;
            char bytes[256];
            auto count = ::read(output_, bytes, sizeof(bytes));
            if(count <= 0)
                return false;
            buffered_.append(bytes, static_cast<std::size_t>(count));
            if(buffered_.size() > 512)
                return false;
        }
    }

    bool retire()
    {
        if(pid_ <= 0)
            return false;
        if(!send("DROP"))
            return false;
        std::string line;
        if(!readLine(line, std::chrono::seconds(10)))
            return false;
        std::istringstream parsed(line);
        std::string marker;
        parsed >> marker >> retireStatus_;
        if(parsed.fail() || marker != "RETIRED" || !send("EXIT"))
            return false;
        int status = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(std::chrono::steady_clock::now() < deadline)
        {
            auto result = ::waitpid(pid_, &status, WNOHANG);
            if(result == pid_)
            {
                bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                pid_ = -1;
                retired_ = clean;
                return clean;
            }
            if(result < 0 && errno != EINTR)
                return false;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }

    bool retired() const { return retired_; }
    int retireStatus() const { return retireStatus_; }

private:
    void cleanup()
    {
        if(pid_ > 0)
        {
            if(input_ >= 0)
                (void)send("DROP");
            int status = 0;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while(std::chrono::steady_clock::now() < deadline)
            {
                auto result = ::waitpid(pid_, &status, WNOHANG);
                if(result == pid_)
                {
                    pid_ = -1;
                    break;
                }
                if(result < 0 && errno != EINTR)
                    break;
                std::this_thread::sleep_for(10ms);
            }
            if(pid_ > 0)
            {
                (void)::kill(pid_, SIGKILL);
                while(::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
                pid_ = -1;
            }
        }
        if(input_ >= 0)
            ::close(input_);
        if(output_ >= 0)
            ::close(output_);
    }

    pid_t pid_{-1};
    int input_{-1};
    int output_{-1};
    ClientId clientId_{};
    bool retired_{};
    int retireStatus_{CL_ERR_UNKNOWN};
    std::string buffered_;
    std::string startupLine_;
};

enum class ProbeStartupFailure
{
    SecondPipe,
    ActionInit,
    Spawn
};

void expectProbeStartupFailureClosesDescriptors(ProbeStartupFailure failure)
{
    std::vector<int> created;
    int pipeCalls = 0;
    PublicProbeStartupCalls calls;
    calls.pipe = [&](int* ends, int flags)
    {
        if(++pipeCalls == 2 && failure == ProbeStartupFailure::SecondPipe)
        {
            errno = EMFILE;
            return -1;
        }
        int result = ::pipe2(ends, flags);
        if(result == 0)
            created.insert(created.end(), ends, ends + 2);
        return result;
    };
    if(failure == ProbeStartupFailure::ActionInit)
        calls.initializeActions = [](posix_spawn_file_actions_t*) noexcept { return ENOMEM; };
    if(failure == ProbeStartupFailure::Spawn)
        calls.spawn = [](pid_t*,
                         char const*,
                         posix_spawn_file_actions_t const*,
                         posix_spawnattr_t const*,
                         char* const*,
                         char* const*) noexcept { return ENOENT; };
    {
        PublicClientProbe probe;
        EXPECT_FALSE(probe.start("ofi+sockets", 1, calls));
    }
    EXPECT_EQ(pipeCalls, 2);
    EXPECT_EQ(created.size(), failure == ProbeStartupFailure::SecondPipe ? 2u : 4u);
    for(int descriptor: created)
    {
        errno = 0;
        EXPECT_EQ(::fcntl(descriptor, F_GETFD), -1) << "leaked pipe descriptor " << descriptor;
        EXPECT_EQ(errno, EBADF);
        // A red regression must not itself leave the intentionally detected FD.
        (void)::close(descriptor);
    }
}

TEST(PublicClientProbeStartup, SecondPipeFailureClosesCreatedDescriptors)
{
    expectProbeStartupFailureClosesDescriptors(ProbeStartupFailure::SecondPipe);
}

TEST(PublicClientProbeStartup, FileActionsInitFailureClosesCreatedDescriptors)
{
    expectProbeStartupFailureClosesDescriptors(ProbeStartupFailure::ActionInit);
}

TEST(PublicClientProbeStartup, SpawnFailureClosesCreatedDescriptors)
{
    expectProbeStartupFailureClosesDescriptors(ProbeStartupFailure::Spawn);
}

struct PublicAcquireResult
{
    int status{};
    long elapsedMs{};
    bool hasHandle{};
};

PublicAcquireResult parseAcquireResult(std::string const& line, std::string const& expected)
{
    std::istringstream parsed(line);
    std::string marker;
    PublicAcquireResult result;
    int handle = 0;
    parsed >> marker >> result.status >> result.elapsedMs >> handle;
    EXPECT_EQ(marker, expected);
    EXPECT_FALSE(parsed.fail());
    result.hasHandle = handle == 1;
    return result;
}

bool waitForCount(std::atomic<unsigned> const& count, unsigned expected, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while(std::chrono::steady_clock::now() < deadline)
    {
        if(count.load(std::memory_order_acquire) >= expected)
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return count.load(std::memory_order_acquire) >= expected;
}
} // namespace

class PublicClientAcquireDeadline: public AdminDeadline
{
protected:
    RpcTimeoutPolicy fixturePolicy() const override { return RpcTimeoutPolicy{}; }
    bool useDefaultAdminCapacity() const override { return true; }
    ServiceId keeperRecordingService(int i) override
    {
        return i == 3 ? service(10 + i) : AdminDeadline::keeperRecordingService(i);
    }

    bool startPublicProbe(VisorClientPortal& portal, PublicClientProbe& probe)
    {
        auto address = std::string(portal.core()->engine->self());
        auto servicePort = static_cast<uint16_t>(std::stoi(address.substr(address.find_last_of(':') + 1)));
        return probe.start(protocol, servicePort);
    }
};

TEST_P(PublicClientAcquireDeadline, C32PortalSaturationTimesOutOnceAndRetainsUnknownClientMutation)
{
    VisorClientPortal portal;
    attach(portal, true);
    PublicClientProbe probe;
    ASSERT_TRUE(startPublicProbe(portal, probe)) << probe.startupLine();

    auto owner = registry->adminOwner();
    auto deadline = rpc::RpcDeadline::after(1s);
    auto permits = occupy(32, deadline);
    ASSERT_EQ(permits.size(), 32u);
    auto rejected33rd = owner->runtime.reserve(rpc::RpcDeadline::after(100ms));
    EXPECT_FALSE(rejected33rd);
    auto incomingBefore = portal.core()->incomingRequests.load(std::memory_order_acquire);

    ASSERT_TRUE(probe.send("ACQUIRE"));
    std::string line;
    ASSERT_TRUE(probe.readLine(line, 7s));
    auto first = parseAcquireResult(line, "RESULT");
    std::cout << "C32 public AcquireStory status=" << first.status << " elapsed_ms=" << first.elapsedMs
              << " handle=" << first.hasHandle << "\n";
    EXPECT_EQ(first.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_GE(first.elapsedMs, 3500);
    EXPECT_LE(first.elapsedMs, 6500);
    EXPECT_FALSE(first.hasHandle);
    EXPECT_EQ(portal.core()->incomingRequests.load(std::memory_order_acquire), incomingBefore);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);

    permits.clear();
    ASSERT_TRUE(probe.send("AGAIN"));
    ASSERT_TRUE(probe.readLine(line, 1s));
    auto second = parseAcquireResult(line, "AGAIN");
    std::cout << "C32 public retry status=" << second.status << " elapsed_ms=" << second.elapsedMs
              << " handle=" << second.hasHandle << "; portal requests and admin starts unchanged\n";
    EXPECT_EQ(second.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_FALSE(second.hasHandle);
    EXPECT_LT(second.elapsedMs, 500);
    EXPECT_EQ(portal.core()->incomingRequests.load(std::memory_order_acquire), incomingBefore);
    for(auto const& count: starts) EXPECT_EQ(count.load(), 0);

    EXPECT_TRUE(probe.retire());
    EXPECT_TRUE(probe.retired());
    EXPECT_EQ(probe.retireStatus(), CL_SUCCESS);
}

TEST_P(PublicClientAcquireDeadline, DefaultAcquireUsesResponsiveKeeperAndAppendsOnlyToReturnedRoster)
{
    delays[1] = 6000;
    delays[2] = 6000;
    delays[3] = 0;
    delays[4] = 0;
    VisorClientPortal portal;
    attach(portal, true);
    PublicClientProbe probe;
    ASSERT_TRUE(startPublicProbe(portal, probe)) << probe.startupLine();

    ASSERT_TRUE(probe.send("ACQUIRE"));
    std::string line;
    ASSERT_TRUE(probe.readLine(line, 7s));
    auto acquired = parseAcquireResult(line, "RESULT");
    std::cout << "degraded public AcquireStory status=" << acquired.status << " elapsed_ms=" << acquired.elapsedMs
              << " handle=" << acquired.hasHandle << " responsive_roster=1\n";
    ASSERT_EQ(acquired.status, CL_SUCCESS);
    ASSERT_TRUE(acquired.hasHandle);
    EXPECT_LT(acquired.elapsedMs, 3500);
    EXPECT_EQ(starts[1].load(), 1);
    EXPECT_EQ(starts[2].load(), 1);
    EXPECT_EQ(starts[3].load(), 1);
    EXPECT_EQ(starts[4].load(), 1);
    {
        std::lock_guard<std::mutex> lock(rosterMutex);
        ASSERT_EQ(playerRoster.size(), 1u);
        EXPECT_EQ(to_string(playerRoster[0]), to_string(keeper(3).getRecordingServiceId()));
    }

    ASSERT_TRUE(probe.send("APPEND"));
    ASSERT_TRUE(probe.readLine(line, 7s));
    std::istringstream appendResult(line);
    std::string marker;
    int status = -1;
    int outcome = -1;
    appendResult >> marker >> status >> outcome;
    EXPECT_EQ(marker, "APPEND");
    EXPECT_EQ(status, CL_SUCCESS);
    EXPECT_EQ(outcome, static_cast<int>(AppendOutcome::Acknowledged));
    std::cout << "public append status=" << status << " outcome=" << outcome
              << " keeper_C_count=" << appendCounts[3].load() << "\n";
    EXPECT_TRUE(waitForCount(appendCounts[3], 1, 2s));
    EXPECT_EQ(appendCounts[1].load(), 0u);
    EXPECT_EQ(appendCounts[2].load(), 0u);

    EXPECT_TRUE(waitForCount(completions[1], 1, 8s));
    EXPECT_TRUE(waitForCount(completions[2], 1, 1s));
    EXPECT_TRUE(probe.retire());
    EXPECT_TRUE(probe.retired());
    EXPECT_EQ(probe.retireStatus(), CL_SUCCESS);
}

TEST_P(PublicClientAcquireDeadline, LostPlayerReplyTimesOutAndPreservesServerAcquisition)
{
    delays[1] = 0;
    delays[2] = 0;
    delays[3] = 0;
    delays[4] = 6000;
    VisorClientPortal portal;
    attach(portal, true);
    PublicClientProbe probe;
    ASSERT_TRUE(startPublicProbe(portal, probe)) << probe.startupLine();

    ASSERT_TRUE(probe.send("ACQUIRE"));
    std::string line;
    ASSERT_TRUE(probe.readLine(line, 7s));
    auto result = parseAcquireResult(line, "RESULT");
    std::cout << "lost-player public AcquireStory status=" << result.status << " elapsed_ms=" << result.elapsedMs
              << " handle=" << result.hasHandle << "\n";
    EXPECT_EQ(result.status, CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_GE(result.elapsedMs, 3500);
    EXPECT_LE(result.elapsedMs, 6500);
    EXPECT_FALSE(result.hasHandle);
    EXPECT_TRUE(waitForCount(completions[4], 1, 8s));
    std::this_thread::sleep_for(100ms);

    auto duplicate = portal.AcquireStory(probe.clientId(), "public-chronicle", "story");
    std::cout << "server duplicate status=" << duplicate.getErrorCode() << " roster=" << duplicate.getKeepers().size()
              << "\n";
    EXPECT_EQ(duplicate.getErrorCode(), CL_ERR_ACQUIRED);
    EXPECT_TRUE(duplicate.getKeepers().empty());
    EXPECT_EQ(starts[1].load(), 1);
    EXPECT_EQ(starts[2].load(), 1);
    EXPECT_EQ(starts[3].load(), 1);
    EXPECT_EQ(starts[4].load(), 1);

    EXPECT_TRUE(probe.retire());
    EXPECT_TRUE(probe.retired());
    EXPECT_EQ(probe.retireStatus(), CL_SUCCESS);
}

INSTANTIATE_TEST_SUITE_P(Public, PublicClientAcquireDeadline, ::testing::Values("ofi+sockets", "ofi+tcp"));
