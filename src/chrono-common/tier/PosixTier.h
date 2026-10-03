#pragma once

#include "tier/FileIO.h"
#include "tier/ManifestLog.h"
#include <atomic>
#include <future>
#include <memory>
#include <thread>

namespace chronolog
{
struct TierConfig
{
    std::string name, kind = "posix";
    std::filesystem::path root;
    uint32_t rank{};
    std::string tier_uuid;
};
struct TierChain
{
    std::string deployment_id;
    std::vector<TierConfig> tiers;
    size_t io_threads = 2;
    std::chrono::milliseconds io_timeout{1000};
};
struct TierDirectory
{
    TierDirectory(int descriptor, uint64_t generation)
        : fd(descriptor)
        , epoch(generation)
    {}
    tier_detail::Fd fd;
    uint64_t epoch;
};
class PosixTier: public std::enable_shared_from_this<PosixTier>
{
public:
    PosixTier(TierConfig config, std::string deployment, size_t threads, std::chrono::milliseconds timeout);
    absl::Status probe();
    std::shared_ptr<TierDirectory> directory() const;
    bool current(const std::shared_ptr<TierDirectory>& directory) const;
    void unavailable();
    absl::Status verify(const TierDirectory& directory) const;
    static absl::StatusOr<std::string> read(int root, const std::string& file, bool direct = false);
    static absl::Status erase(int root, const std::string& file);
    static absl::StatusOr<std::vector<std::string>> list(int root, const std::string& directory);
    const TierConfig config;
    template <class F>
    auto submit(F task) -> std::future<decltype(task())>
    {
        using Result = decltype(task());
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        auto count = active_;
        auto value = count->load();
        do {
            if(value >= threads_)
            {
                promise->set_value(Result(absl::UnavailableError("tier executor is full")));
                return future;
            }
        } while(!count->compare_exchange_weak(value, value + 1));
        try
        {
            std::thread(
                    [promise, count, task = std::move(task)]() mutable
                    {
                        try
                        {
                            promise->set_value(task());
                        }
                        catch(const std::exception& error)
                        {
                            promise->set_value(Result(absl::UnavailableError(error.what())));
                        }
                        --*count;
                    })
                    .detach();
        }
        catch(...)
        {
            --*count;
            throw;
        }
        return future;
    }
    template <class F>
    auto run(F task) -> decltype(task())
    {
        auto future = submit(std::move(task));
        if(future.wait_for(timeout_) != std::future_status::ready)
        {
            unavailable();
            return decltype(task())(absl::UnavailableError("tier I/O deadline expired"));
        }
        return future.get();
    }

private:
    std::string deployment_;
    size_t threads_;
    std::chrono::milliseconds timeout_;
    std::shared_ptr<std::atomic<size_t>> active_ = std::make_shared<std::atomic<size_t>>(0);
    std::atomic<bool> probing_{};
    mutable std::mutex mutex_;
    uint64_t epoch_{};
    std::shared_ptr<TierDirectory> directory_;
};
} // namespace chronolog
