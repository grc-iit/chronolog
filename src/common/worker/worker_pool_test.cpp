#include "worker/WorkerPool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <future>

namespace chronolog
{
namespace
{

TEST(WorkerPool, TasksRunOnWorkerThreads)
{
    EXPECT_FALSE(WorkerPool::onWorkerThread());
    WorkerPool pool(2, 8);
    std::promise<bool> on_worker;
    ASSERT_TRUE(pool.submit([&] { on_worker.set_value(WorkerPool::onWorkerThread()); }));
    EXPECT_TRUE(on_worker.get_future().get());
}

TEST(WorkerPool, FullQueueRefusesAndStopDrainsQueuedTasks)
{
    WorkerPool pool(1, 1);
    std::promise<void> started, release;
    auto released = release.get_future().share();
    ASSERT_TRUE(pool.submit(
            [&, released]
            {
                started.set_value();
                released.wait();
            }));
    started.get_future().wait();
    std::atomic<int> ran{0};
    ASSERT_TRUE(pool.submit([&] { ++ran; }));
    EXPECT_FALSE(pool.submit([&] { ++ran; }));
    release.set_value();
    pool.stop();
    EXPECT_EQ(ran.load(), 1);
    EXPECT_FALSE(pool.submit([&] { ++ran; }));
    pool.stop();
}

TEST(WorkerPool, ExpiredTaskIsRefusedAndCounted)
{
    WorkerPool pool(1, 8);
    std::atomic<int> ran{0}, refused{0};
    ASSERT_TRUE(pool.submit([&] { ++ran; }, [] { return true; }, [&] { ++refused; }));
    ASSERT_TRUE(pool.submit([&] { ++ran; }, [] { return false; }, [&] { ++refused; }));
    pool.stop();
    EXPECT_EQ(ran.load(), 1);
    EXPECT_EQ(refused.load(), 1);
    EXPECT_EQ(pool.dropped(), 1u);
}

} // namespace
} // namespace chronolog
