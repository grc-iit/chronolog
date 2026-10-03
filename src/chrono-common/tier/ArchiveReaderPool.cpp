#include "tier/ArchiveReaderPool.h"

namespace chronolog
{
ArchiveReaderPool::ArchiveReaderPool(size_t threads)
    : max_queue_(2 * threads)
{
    try
    {
        workers_.reserve(threads);
        for(size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { run(); });
    }
    catch(...)
    {
        stop();
        throw;
    }
}

ArchiveReaderPool::~ArchiveReaderPool() { stop(); }

void ArchiveReaderPool::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    work_.notify_all();
    room_.notify_all();
    workers_.clear();
}

bool ArchiveReaderPool::submit(std::function<void()> task)
{
    {
        std::unique_lock lock(mutex_);
        room_.wait(lock, [&] { return stopping_ || queue_.size() < max_queue_; });
        if(stopping_)
            return false;
        queue_.push_back(std::move(task));
    }
    work_.notify_one();
    return true;
}

void ArchiveReaderPool::run()
{
    while(true)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            work_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if(queue_.empty())
                return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        room_.notify_one();
        task();
    }
}
} // namespace chronolog
