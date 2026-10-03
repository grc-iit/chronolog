#pragma once

#include <atomic>
#include <chrono>
#include <grpcpp/grpcpp.h>
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
    // A task for a call that can expire while queued: a worker that dequeues it after `expired` turns true runs
    // `refuse` instead, so a backlog of dead calls drains at answer cost and never becomes congestion collapse.
    bool submit(std::function<void()> task, std::function<bool()> expired, std::function<void()> refuse);
    // Tasks answered without running because their call had already expired.
    uint64_t dropped() const { return dropped_.load(); }

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

// Queues a unary call's work; a call already cancelled or past its deadline when a worker reaches it is answered
// CANCELLED or DEADLINE_EXCEEDED without running.
inline bool SubmitCall(WorkerPool& pool,
                       grpc::CallbackServerContext* context,
                       grpc::ServerUnaryReactor* reactor,
                       std::function<void()> task)
{
    return pool.submit(
            std::move(task),
            [context] { return context->IsCancelled() || std::chrono::system_clock::now() >= context->deadline(); },
            [context, reactor]
            {
                reactor->Finish(context->IsCancelled() ? grpc::Status::CANCELLED
                                                       : grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                                                      "deadline passed while queued"));
            });
}

} // namespace chronolog::visor
