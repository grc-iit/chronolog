#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace chronolog
{
class ArchiveReaderPool
{
public:
    explicit ArchiveReaderPool(size_t threads);
    ~ArchiveReaderPool();
    bool submit(std::function<void()> task);

private:
    void stop();
    void run();
    const size_t max_queue_;
    std::mutex mutex_;
    std::condition_variable work_, room_;
    std::deque<std::function<void()>> queue_;
    bool stopping_{};
    std::vector<std::jthread> workers_;
};
} // namespace chronolog
