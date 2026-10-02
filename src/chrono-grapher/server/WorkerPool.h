#pragma once

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace chronolog::grapher
{
// Bounded std::jthread pool. The manifest read, the HDF5 write and the fsync of a published chunk run here,
// never on a gRPC thread (M11.7). stop() runs every queued task, then joins.
class WorkerPool
{
public:
    WorkerPool(size_t threads, size_t max_queue);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    // Returns false when the queue is full or the pool is stopping.
    bool submit(std::function<void()> task);
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
} // namespace chronolog::grapher

#define CHRONOLOG_ASSERT_WORKER_THREAD() assert(::chronolog::grapher::WorkerPool::onWorkerThread())
