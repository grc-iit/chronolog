#include "common/tier/ArchiveReaderPool.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace chronolog
{
struct ArchiveReaderPool::Worker
{
    std::jthread thread;
    bool busy{}, retired{};
    Deadline deadline;
};
struct ArchiveReaderPool::State
{
    struct Task
    {
        std::function<void()> run;
        Deadline deadline;
    };
    std::mutex mutex;
    std::condition_variable work;
    std::deque<Task> queue;
    bool stopping{};
    size_t abandoned{}, busy{};
    size_t capacity{};
};

ArchiveReaderPool::ArchiveReaderPool(size_t threads)
    : threads_(threads)
    , state_(std::make_shared<State>())
{
    try
    {
        state_->capacity = threads;
        workers_.reserve(threads);
        for(size_t i = 0; i < threads; ++i) startWorker();
    }
    catch(...)
    {
        stop();
        throw;
    }
}
ArchiveReaderPool::~ArchiveReaderPool() { stop(); }

void ArchiveReaderPool::startWorker()
{
    auto worker = std::make_shared<Worker>();
    workers_.push_back(worker);
    worker->thread = std::jthread(
            [state = state_, worker]
            {
                std::unique_lock lock(state->mutex);
                while(!state->stopping)
                {
                    state->work.wait(lock,
                                     [&] {
                                         return state->stopping || (!state->queue.empty() &&
                                                                    state->busy + state->abandoned < state->capacity);
                                     });
                    if(state->stopping)
                        break;
                    auto task = std::move(state->queue.front());
                    state->queue.pop_front();
                    if(std::chrono::steady_clock::now() >= task.deadline)
                        continue;
                    worker->busy = true;
                    ++state->busy;
                    worker->deadline = task.deadline;
                    lock.unlock();
                    try
                    {
                        task.run();
                    }
                    catch(...)
                    {}
                    lock.lock();
                    worker->busy = false;
                    if(worker->retired)
                    {
                        --state->abandoned;
                        state->work.notify_all();
                        return;
                    }
                    --state->busy;
                    state->work.notify_all();
                }
            });
}

void ArchiveReaderPool::expireLocked()
{
    const auto now = std::chrono::steady_clock::now();
    for(auto it = workers_.begin(); it != workers_.end();)
    {
        auto worker = *it;
        if(worker->busy && worker->deadline <= now && state_->abandoned < threads_)
        {
            worker->retired = true;
            ++state_->abandoned;
            --state_->busy;
            worker->thread.detach();
            it = workers_.erase(it);
        }
        else
            ++it;
    }
    if(state_->abandoned < threads_)
        while(workers_.size() < threads_) startWorker();
}

void ArchiveReaderPool::expire()
{
    std::lock_guard lock(state_->mutex);
    if(!state_->stopping)
        expireLocked();
}

bool ArchiveReaderPool::submit(std::function<void()> task, Deadline deadline)
{
    std::lock_guard lock(state_->mutex);
    if(state_->stopping || std::chrono::steady_clock::now() >= deadline)
        return false;
    expireLocked();
    if(state_->abandoned >= threads_ || state_->queue.size() >= 2 * threads_)
        return false;
    state_->queue.push_back({std::move(task), deadline});
    state_->work.notify_one();
    return true;
}

void ArchiveReaderPool::stop()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->queue.clear();
        for(auto& worker: workers_)
            if(worker->busy)
            {
                worker->retired = true;
                ++state_->abandoned;
                --state_->busy;
                worker->thread.detach();
            }
    }
    state_->work.notify_all();
    for(auto& worker: workers_)
        if(worker->thread.joinable())
            worker->thread.join();
    workers_.clear();
}
} // namespace chronolog
