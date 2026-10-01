#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "catalog/AcquisitionLedger.h"

namespace chronolog::visor
{

// Fans committed acquisition changes out to Cluster.WatchAcquisitions streams. The
// feed knows nothing about gRPC; a stream sends a Subscription's snapshot and then
// drains its queue of later changes.
class AcquisitionFeed final: public AcquisitionObserver
{
public:
    class Subscription
    {
    public:
        // An empty `keeper_id` matches every acquisition, otherwise only those
        // assigned to that Keeper process.
        Subscription(size_t capacity, std::string keeper_id)
            : capacity_(capacity)
            , keeper_id_(std::move(keeper_id))
        {}

        // Active acquisitions assigned to the subscriber as of snapshot().revision.
        AcquisitionSnapshot snapshot() const;
        // Next queued change above the snapshot revision, or nullopt when empty.
        std::optional<AcquisitionChange> pop();
        // True once the consumer fell more than `capacity` changes behind. The
        // consumer must end the stream so the Keeper resubscribes and gets a snapshot.
        bool overflowed() const;
        // The callback runs without any feed lock held and must not block. Pass
        // nullptr to detach.
        void setWakeup(std::function<void()> wakeup);
        // Used by the feed and its tests.
        void push(const AcquisitionChange& change);
        void seed(const AcquisitionSnapshot& snapshot);

    private:
        bool matches(const AcquisitionChange& change) const;

        const size_t capacity_;
        const std::string keeper_id_;
        mutable std::mutex mutex_;
        AcquisitionSnapshot snapshot_;
        std::deque<AcquisitionChange> queue_;
        bool overflowed_{};
        std::function<void()> wakeup_;
    };

    explicit AcquisitionFeed(size_t subscription_capacity = 10000);

    // Registers for changes first and then takes the ledger snapshot, so no change
    // is lost between the two. Changes already covered by the snapshot are dropped.
    absl::StatusOr<std::shared_ptr<Subscription>> subscribe(const AcquisitionLedger& ledger,
                                                            const std::string& keeper_id = {});

    void onAcquisitionChange(const AcquisitionChange& change) override;

private:
    const size_t capacity_;
    std::mutex mutex_;
    std::vector<std::weak_ptr<Subscription>> subscriptions_;
};

} // namespace chronolog::visor
