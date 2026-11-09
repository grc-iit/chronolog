#include <gtest/gtest.h>
#include <private/LifecyclePublisher.h>
#include <private/ServiceLifecycle.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
namespace
{
namespace rpc = chronolog::rpc;
using namespace std::chrono_literals;
using Clock = rpc::RpcDeadline::Clock;
using Record = rpc::LifecyclePublisher::Record;

std::size_t threadCount()
{
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/task"),
                                                  std::filesystem::directory_iterator{}));
}
bool until(std::function<bool()> condition, std::chrono::milliseconds wait = 3000ms)
{
    auto end = Clock::now() + wait;
    while(!condition())
    {
        if(Clock::now() >= end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}
// Stands in for the status file. Each call is one write; a held call blocks
// its worker the way a stalled file system or logger would.
struct Sink
{
    std::atomic<bool> hold{false};
    std::atomic<int> failures{0};
    std::atomic<unsigned> calls{0};
    std::mutex mutex;
    std::vector<std::pair<rpc::ShutdownState, uint64_t>> written;
    rpc::LifecyclePublisher::Writer writer()
    {
        return [this](Record const& record)
        {
            ++calls;
            while(hold.load()) std::this_thread::sleep_for(1ms);
            if(failures.load() > 0)
            {
                --failures;
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex);
            written.emplace_back(record.state, record.attempt);
            return true;
        };
    }
    std::pair<rpc::ShutdownState, uint64_t> last()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return written.empty() ? std::pair<rpc::ShutdownState, uint64_t>{} : written.back();
    }
};
Record quiescing(uint64_t attempt)
{
    return rpc::LifecyclePublisher::record(rpc::ShutdownState::Quiescing,
                                           chronolog::CL_ERR_SHUTDOWN_INCOMPLETE,
                                           attempt,
                                           Clock::now(),
                                           {},
                                           0);
}
} // namespace

// A held write keeps its one step and runtime slot. The caller returns at its
// own deadline, later attempts admit nothing new, and after release the next
// attempt writes the newest record on the same runtime without new threads.
TEST(LifecyclePublisher, HeldWriteKeepsOneStepAndReturnsAtTheCallerDeadline)
{
    auto runtime = std::make_shared<rpc::RpcRuntime>(2);
    auto root = std::make_shared<int>(0);
    Sink sink;
    auto publisher = rpc::LifecyclePublisher::Create(sink.writer());
    sink.hold = true;
    publisher->beginAttempt(1, Clock::now());
    auto first = publisher->request(quiescing(1));
    auto entered = Clock::now();
    auto deadline = rpc::RpcDeadline::after(150ms);
    publisher->pump(*runtime, deadline, root);
    ASSERT_TRUE(until([&] { return sink.calls == 1; }));
    auto threads = threadCount();
    EXPECT_FALSE(publisher->waitPublished(first, *runtime, deadline, root));
    auto elapsed = Clock::now() - entered;
    EXPECT_GE(elapsed, 140ms);
    EXPECT_LT(elapsed, 400ms);
    // The held step still occupies one of the two slots.
    EXPECT_FALSE(rpc::allControlIdle(*runtime, 2, rpc::RpcDeadline::after(50ms)));

    publisher->beginAttempt(2, Clock::now());
    auto second = publisher->request(quiescing(2));
    auto retry = rpc::RpcDeadline::after(100ms);
    for(int i = 0; i < 20; ++i) publisher->pump(*runtime, retry, root);
    EXPECT_FALSE(publisher->waitPublished(second, *runtime, retry, root));
    EXPECT_EQ(1u, sink.calls.load());
    EXPECT_LE(threadCount(), threads); // No growth; workers of earlier runtimes may exit.

    sink.hold = false;
    ASSERT_TRUE(until([&] { return publisher->published(first); }));
    EXPECT_EQ(1u, sink.calls.load());
    // The second attempt's deadline has passed: nothing is admitted for it.
    publisher->pump(*runtime, retry, root);
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(1u, sink.calls.load());

    publisher->beginAttempt(3, Clock::now());
    auto third = publisher->request(quiescing(3));
    EXPECT_TRUE(publisher->waitPublished(third, *runtime, rpc::RpcDeadline::after(1s), root));
    EXPECT_EQ(2u, sink.calls.load());
    EXPECT_EQ(3u, sink.last().second);
    EXPECT_TRUE(rpc::allControlIdle(*runtime, 2, rpc::RpcDeadline::after(1s)));
    EXPECT_LE(threadCount(), threads); // No growth; workers of earlier runtimes may exit.
}

TEST(LifecyclePublisher, ExpiredDeadlineAdmitsNoWrite)
{
    auto runtime = std::make_shared<rpc::RpcRuntime>(2);
    auto root = std::make_shared<int>(0);
    Sink sink;
    auto publisher = rpc::LifecyclePublisher::Create(sink.writer());
    auto sequence = publisher->request(quiescing(1));
    auto deadline = rpc::RpcDeadline::after(1ms);
    std::this_thread::sleep_for(5ms);
    publisher->pump(*runtime, deadline, root);
    EXPECT_FALSE(publisher->waitPublished(sequence, *runtime, deadline, root));
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(0u, sink.calls.load());
}

// A known failed write is not repeated by later pumps; a new request is.
TEST(LifecyclePublisher, FailedWriteIsRetriedOnlyForANewRequest)
{
    auto runtime = std::make_shared<rpc::RpcRuntime>(2);
    auto root = std::make_shared<int>(0);
    Sink sink;
    sink.failures = 1;
    auto publisher = rpc::LifecyclePublisher::Create(sink.writer());
    auto first = publisher->request(quiescing(1));
    auto deadline = rpc::RpcDeadline::after(1s);
    EXPECT_FALSE(publisher->waitPublished(first, *runtime, deadline, root));
    for(int i = 0; i < 20; ++i)
    {
        publisher->pump(*runtime, deadline, root);
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(1u, sink.calls.load());
    auto second = publisher->request(quiescing(1));
    EXPECT_TRUE(publisher->waitPublished(second, *runtime, deadline, root));
    EXPECT_EQ(2u, sink.calls.load());
}

// The terminal write runs in a real owner cleanup callback. A known failure
// keeps the callback, its slot and the same content; only a new stop attempt
// tries once more. The runtime drain stays incomplete until it succeeds.
TEST(LifecyclePublisher, TerminalFailureRetriesOncePerNewAttemptInTheOwnerCallback)
{
    Sink sink;
    sink.failures = 2;
    auto publisher = rpc::LifecyclePublisher::Create(sink.writer());
    auto runtime = std::make_shared<rpc::RpcRuntime>(2);
    auto root = std::make_shared<int>(0);
    runtime->retainOwner(root,
                         [publisher]
                         {
                             publisher->publishTerminal(rpc::LifecyclePublisher::record(rpc::ShutdownState::Stopped,
                                                                                        chronolog::CL_SUCCESS,
                                                                                        0,
                                                                                        {},
                                                                                        {},
                                                                                        0));
                         });
    auto threads = threadCount();
    publisher->beginAttempt(1, Clock::now());
    EXPECT_EQ(chronolog::CL_ERR_SHUTDOWN_INCOMPLETE, runtime->drain(rpc::RpcDeadline::after(100ms)));
    EXPECT_EQ(1u, sink.calls.load());
    EXPECT_FALSE(publisher->terminalPublished());

    publisher->beginAttempt(2, Clock::now());
    EXPECT_EQ(chronolog::CL_ERR_SHUTDOWN_INCOMPLETE, runtime->drain(rpc::RpcDeadline::after(100ms)));
    EXPECT_EQ(2u, sink.calls.load());
    EXPECT_LE(threadCount(), threads); // No growth; workers of earlier runtimes may exit.

    publisher->beginAttempt(3, Clock::now());
    EXPECT_EQ(chronolog::CL_SUCCESS, runtime->drain(rpc::RpcDeadline::after(2s)));
    EXPECT_TRUE(publisher->terminalPublished());
    EXPECT_EQ(3u, sink.calls.load());
    EXPECT_EQ(rpc::ShutdownState::Stopped, sink.last().first);
    EXPECT_EQ(3u, sink.last().second);
}

TEST(LifecyclePublisher, HeldTerminalWriteIsNeverRetried)
{
    Sink sink;
    sink.hold = true;
    auto publisher = rpc::LifecyclePublisher::Create(sink.writer());
    auto runtime = std::make_shared<rpc::RpcRuntime>(2);
    auto root = std::make_shared<int>(0);
    runtime->retainOwner(root,
                         [publisher]
                         {
                             publisher->publishTerminal(rpc::LifecyclePublisher::record(rpc::ShutdownState::Stopped,
                                                                                        chronolog::CL_SUCCESS,
                                                                                        0,
                                                                                        {},
                                                                                        {},
                                                                                        0));
                         });
    publisher->beginAttempt(1, Clock::now());
    EXPECT_EQ(chronolog::CL_ERR_SHUTDOWN_INCOMPLETE, runtime->drain(rpc::RpcDeadline::after(100ms)));
    ASSERT_EQ(1u, sink.calls.load());
    publisher->beginAttempt(2, Clock::now());
    EXPECT_EQ(chronolog::CL_ERR_SHUTDOWN_INCOMPLETE, runtime->drain(rpc::RpcDeadline::after(100ms)));
    EXPECT_EQ(1u, sink.calls.load());
    EXPECT_FALSE(publisher->terminalPublished());
    sink.hold = false;
    EXPECT_EQ(chronolog::CL_SUCCESS, runtime->drain(rpc::RpcDeadline::after(2s)));
    EXPECT_TRUE(publisher->terminalPublished());
    EXPECT_EQ(1u, sink.calls.load());
}

// The terminal write never overtakes a nonterminal write still in progress.
TEST(LifecyclePublisher, TerminalWaitsForTheActiveNonterminalWrite)
{
    Sink sink;
    auto publisher = rpc::LifecyclePublisher::Create(sink.writer());
    auto status = std::make_shared<rpc::RpcRuntime>(2);
    auto root = std::make_shared<int>(0);
    sink.hold = true;
    publisher->beginAttempt(1, Clock::now());
    publisher->request(quiescing(1));
    publisher->pump(*status, rpc::RpcDeadline::after(1s), root);
    ASSERT_TRUE(until([&] { return sink.calls == 1; }));
    std::thread terminal(
            [&]
            {
                publisher->publishTerminal(rpc::LifecyclePublisher::record(rpc::ShutdownState::Stopped,
                                                                           chronolog::CL_SUCCESS,
                                                                           0,
                                                                           {},
                                                                           {},
                                                                           0));
            });
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(1u, sink.calls.load());
    EXPECT_FALSE(publisher->terminalPublished());
    sink.hold = false;
    terminal.join();
    EXPECT_TRUE(publisher->terminalPublished());
    ASSERT_EQ(2u, sink.written.size());
    EXPECT_EQ(rpc::ShutdownState::Quiescing, sink.written[0].first);
    EXPECT_EQ(rpc::ShutdownState::Stopped, sink.written[1].first);
}
