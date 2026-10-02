#include "chrono-grapher/server/WorkerPool.h"

#include <utility>

namespace chronolog::grapher
{
namespace
{
thread_local bool tls_worker = false;
}

WorkerPool::WorkerPool(size_t threads, size_t max_queue)
    : max_queue_(max_queue)
{
    workers_.reserve(threads);
    for(size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    workers_.clear();
}

bool WorkerPool::submit(std::function<void()> task)
{
    {
        std::lock_guard lock(mutex_);
        if(stopping_ || queue_.size() >= max_queue_)
            return false;
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

bool WorkerPool::onWorkerThread() { return tls_worker; }

void WorkerPool::run()
{
    tls_worker = true;
    while(true)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if(queue_.empty())
                return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}
} // namespace chronolog::grapher
