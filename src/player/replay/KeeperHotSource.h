#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include "player/replay/RouteSource.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

namespace chronolog::player
{

struct KeeperHotSourceOptions
{
    std::chrono::milliseconds deadline{2000};
    // Zero lets each Keeper apply its configured default limit.
    uint64_t max_events{0};
    size_t read_max_events{262144};
};

// Asks every Keeper in the story Route for its hot data through Archive.FetchHot. A Keeper
// that fails or times out comes back answered=false, so the read is incomplete rather than wrong.
class KeeperHotSource final: public HotSource
{
public:
    // `internal_address` maps a Route Keeper to the address of its internal listener.
    KeeperHotSource(std::shared_ptr<const RouteSource> routes,
                    std::shared_ptr<const WriterSource> writers,
                    std::function<std::string(const KeeperRef&)> internal_address,
                    KeeperHotSourceOptions options = {});

    // Connects to a Keeper listener now, while name resolution works, so a later resolver outage on the
    // network does not reach reads that find the connection already up.
    void warm(const std::string& address) const;
    std::shared_ptr<KeeperHotSource> bounded(std::chrono::system_clock::time_point deadline) const;

    absl::StatusOr<HotFetch> fetch(StoryId story, const Range& range) const override;
    absl::StatusOr<HotFetch> fetchRead(StoryId story, const Range& range, size_t target) const override;
    absl::StatusOr<HotFetch> fetchPhysical(StoryId story, const Range& range, bool policy) const override;
    absl::StatusOr<HotFetch> fetchTail(StoryId story, Hlc from, const TailStarts& starts) const override;

    absl::StatusOr<internal::v1::WriterStatusResponse>
    writerStatus(EventId id, std::chrono::system_clock::time_point deadline) const;

private:
    absl::StatusOr<HotFetch>
    fetchImpl(StoryId story, const Range& range, bool policy, const TailStarts* starts, size_t read_budget = 0) const;
    KeeperFetch fetchOne(const KeeperRef& keeper,
                         StoryId story,
                         const Range& range,
                         Epoch expected_epoch,
                         const Predecessor* predecessor,
                         std::atomic<size_t>& retained,
                         bool policy,
                         bool tail,
                         size_t read_budget,
                         std::chrono::system_clock::time_point deadline) const;
    std::shared_ptr<internal::v1::Archive::Stub> stubFor(const std::string& address) const;

    std::shared_ptr<const RouteSource> routes_;
    std::shared_ptr<const WriterSource> writers_;
    std::function<std::string(const KeeperRef&)> internal_address_;
    KeeperHotSourceOptions options_;
    mutable std::mutex mu_;
    mutable std::map<std::string, std::shared_ptr<internal::v1::Archive::Stub>> stubs_;
};

} // namespace chronolog::player
