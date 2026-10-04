#include "worker/WorkerPool.h"

#include <utility>

namespace chronolog
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

bool WorkerPool::submit(std::function<void()> task) { return submit(std::move(task), {}, {}); }

bool WorkerPool::submit(std::function<void()> task, std::function<bool()> expired, std::function<void()> refuse)
{
    {
        std::lock_guard lock(mutex_);
        if(stopping_ || queue_.size() >= max_queue_)
            return false;
        queue_.push_back({std::move(task), std::move(refuse), std::move(expired)});
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
        Item item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if(queue_.empty())
                return;
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        if(item.expired && item.expired())
        {
            ++dropped_;
            item.refuse();
        }
        else
            item.task();
    }
}

} // namespace chronolog
