#pragma once

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace chronolog
{

// Bounded std::jthread pool. Blocking work (Journal calls, fsync, HDF5, SQLite) runs here, never on a gRPC
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
    // A task for a call that can expire while queued: a worker that dequeues it after `expired` turns true runs
    // `refuse` instead, so a backlog of dead calls drains at answer cost and never becomes congestion collapse.
    bool submit(std::function<void()> task, std::function<bool()> expired, std::function<void()> refuse);
    // Idempotent.
    void stop();

    // Tasks answered without running because their call had already expired.
    uint64_t dropped() const { return dropped_.load(); }

    static bool onWorkerThread();

private:
    void run();

    const size_t max_queue_;
    std::atomic<uint64_t> dropped_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    struct Item
    {
        std::function<void()> task, refuse;
        std::function<bool()> expired;
    };
    std::deque<Item> queue_;
    bool stopping_{};
    std::vector<std::jthread> workers_;
};

} // namespace chronolog

#define CHRONOLOG_ASSERT_WORKER_THREAD() assert(::chronolog::WorkerPool::onWorkerThread())
