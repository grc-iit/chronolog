#pragma once

#include "common/tier/FileIO.h"
#include "common/tier/ArchiveReaderPool.h"
#include "common/tier/ManifestLog.h"
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
    absl::Status probe(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));
    void stop() { executor_.reset(); }
    std::shared_ptr<TierDirectory> directory() const;
    bool current(const std::shared_ptr<TierDirectory>& directory) const;
    // Probe ordinals (I13.15): a slow-tier LOST verdict repeats only under a probe that started after the first
    // verdict and verified the root. probesStarted counts every probe that got past the one-outstanding rule.
    uint64_t probesStarted() const;
    uint64_t lastVerifiedProbe() const;
    void unavailable();
    void expire()
    {
        unavailable();
        executor_->expire();
    }
    std::chrono::milliseconds timeout() const { return timeout_; }
    absl::Status verify(const TierDirectory& directory) const;
    static absl::StatusOr<std::string> read(int root, const std::string& file, bool direct = false);
    static absl::Status erase(int root, const std::string& file);
    static absl::StatusOr<std::vector<std::string>> list(int root, const std::string& directory);
    const TierConfig config;
    template <class F>
    auto submit(F task,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) -> std::future<decltype(task())>
    {
        using Result = decltype(task());
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        const auto deadline = std::chrono::steady_clock::now() + (timeout.count() > 0 ? timeout : timeout_);
        if(!executor_->submit(
                   [promise, task = std::optional<F>(std::move(task))]() mutable
                   {
                       std::optional<Result> result;
                       try
                       {
                           result.emplace((*task)());
                       }
                       catch(const std::exception& error)
                       {
                           result.emplace(absl::UnavailableError(error.what()));
                       }
                       task.reset();
                       promise->set_value(std::move(*result));
                   },
                   deadline))
            promise->set_value(Result(absl::UnavailableError("tier executor is full")));
        return future;
    }
    template <class F>
    auto run(F task, std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) -> decltype(task())
    {
        auto future = submit(std::move(task), timeout);
        if(future.wait_for(timeout.count() > 0 ? timeout : timeout_) != std::future_status::ready)
        {
            expire();
            return decltype(task())(absl::UnavailableError("tier I/O deadline expired"));
        }
        try
        {
            return future.get();
        }
        catch(const std::exception& error)
        {
            return decltype(task())(absl::UnavailableError(error.what()));
        }
    }

private:
    std::string deployment_;
    std::chrono::milliseconds timeout_;
    std::unique_ptr<ArchiveReaderPool> executor_;
    std::atomic<bool> probing_{};
    mutable std::mutex mutex_;
    uint64_t epoch_{};
    uint64_t probes_started_{}, last_verified_probe_{};
    std::shared_ptr<TierDirectory> directory_;
};
} // namespace chronolog
