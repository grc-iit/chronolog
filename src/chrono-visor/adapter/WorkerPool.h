#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace chronolog::visor
{

// Bounded std::jthread pool. gRPC callbacks enqueue here so that SQLite fsync never
// blocks a gRPC thread (M11.4, M11.7). The destructor runs every queued task and
// then joins.
class WorkerPool
{
public:
    WorkerPool(size_t threads, size_t max_queue);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Returns false when the queue is full or the pool is stopping.
    bool submit(std::function<void()> task);
    // Tasks answered without running because their call had already expired.
    uint64_t dropped() const { return dropped_.load(); }

private:
    void run();

    const size_t max_queue_;
    std::atomic<uint64_t> dropped_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stopping_{};
    std::vector<std::jthread> workers_;
};

} // namespace chronolog::visor
