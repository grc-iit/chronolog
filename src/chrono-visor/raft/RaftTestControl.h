#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace chronolog::visor
{
class RaftTestControl
{
public:
    std::atomic<uint64_t> proposal_count{};
    std::atomic<bool> qualification_enabled{true};
    std::atomic<bool> suppress_reply{};
    void holdNextRebuild()
    {
        std::lock_guard lock(mutex_);
        rebuild_hold_ = true;
        captured_ = false;
    }
    bool waitRebuild(std::chrono::milliseconds budget)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, budget, [&] { return captured_; });
    }
    void snapshotCaptured(uint64_t index)
    {
        std::unique_lock lock(mutex_);
        if(!rebuild_hold_)
            return;
        captured_index = index;
        captured_ = true;
        cv_.notify_all();
        if(!cv_.wait_for(lock, std::chrono::seconds(12), [&] { return !rebuild_hold_; }))
            throw std::runtime_error("Raft test rebuild barrier exceeded its budget");
    }
    void releaseRebuild()
    {
        std::lock_guard lock(mutex_);
        rebuild_hold_ = false;
        cv_.notify_all();
    }
    uint64_t captured_index{};
    void holdNextApply()
    {
        std::lock_guard lock(mutex_);
        hold_ = true;
        accepted_ = 0;
    }
    std::string lastCommand()
    {
        std::lock_guard lock(mutex_);
        return last_command_;
    }
    void accepted(uint64_t index, std::string command)
    {
        std::lock_guard lock(mutex_);
        last_command_ = std::move(command);
        if(hold_ && !accepted_)
        {
            accepted_ = index;
            cv_.notify_all();
        }
    }
    uint64_t waitAccepted(std::chrono::milliseconds budget)
    {
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, budget, [&] { return accepted_ != 0; });
        return accepted_;
    }
    void beforeApply(uint64_t index)
    {
        std::unique_lock lock(mutex_);
        if(hold_ && accepted_ == index && !cv_.wait_for(lock, std::chrono::seconds(12), [&] { return !hold_; }))
            throw std::runtime_error("Raft test apply barrier exceeded its budget");
    }
    void releaseApply()
    {
        std::lock_guard lock(mutex_);
        hold_ = false;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool hold_{};
    uint64_t accepted_{};
    bool rebuild_hold_{};
    bool captured_{};
    std::string last_command_;
};
} // namespace chronolog::visor
