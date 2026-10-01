#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace chronolog::keeper
{

// Bounded std::jthread pool. Every call into the Journal runs here, never on a gRPC
// thread (M11.7). stop() runs every queued task, then joins.
class WorkerPool
{
public:
    WorkerPool(size_t threads, size_t max_queue);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Returns false when the queue is full or the pool is stopping.
    bool submit(std::function<void()> task);
    // Idempotent.
    void stop();

    static bool onWorkerThread();

private:
    void run();

    const size_t max_queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stopping_{};
    std::vector<std::jthread> workers_;
};

} // namespace chronolog::keeper

#define CHRONOLOG_ASSERT_WORKER_THREAD() assert(::chronolog::keeper::WorkerPool::onWorkerThread())
