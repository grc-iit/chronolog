#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace chronolog
{
class ArchiveReaderPool
{
public:
    using Deadline = std::chrono::steady_clock::time_point;
    explicit ArchiveReaderPool(size_t threads);
    ~ArchiveReaderPool();
    bool submit(std::function<void()> task, Deadline deadline);
    void expire();

private:
    struct State;
    struct Worker;
    void startWorker();
    void expireLocked();
    void stop();
    const size_t threads_;
    std::shared_ptr<State> state_;
    std::vector<std::shared_ptr<Worker>> workers_;
};
} // namespace chronolog
