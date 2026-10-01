#include "catalog/AcquisitionFeed.h"

#include <algorithm>
#include <utility>

namespace chronolog::visor
{

std::optional<AcquisitionChange> AcquisitionFeed::Subscription::pop()
{
    std::lock_guard lock(mutex_);
    if(queue_.empty())
        return std::nullopt;
    AcquisitionChange change = std::move(queue_.front());
    queue_.pop_front();
    return change;
}

bool AcquisitionFeed::Subscription::overflowed() const
{
    std::lock_guard lock(mutex_);
    return overflowed_;
}

void AcquisitionFeed::Subscription::setWakeup(std::function<void()> wakeup)
{
    std::lock_guard lock(mutex_);
    wakeup_ = std::move(wakeup);
}

void AcquisitionFeed::Subscription::push(const AcquisitionChange& change)
{
    std::function<void()> wakeup;
    {
        std::lock_guard lock(mutex_);
        if(overflowed_)
            return;
        if(queue_.size() >= capacity_)
        {
            overflowed_ = true;
            queue_.clear();
        }
        else
        {
            queue_.push_back(change);
        }
        wakeup = wakeup_;
    }
    if(wakeup)
        wakeup();
}

void AcquisitionFeed::Subscription::seed(const AcquisitionSnapshot& snapshot)
{
    std::lock_guard lock(mutex_);
    std::erase_if(queue_, [&](const AcquisitionChange& c) { return c.revision <= snapshot.revision; });
    queue_.insert(queue_.begin(), snapshot.active.begin(), snapshot.active.end());
}

AcquisitionFeed::AcquisitionFeed(size_t subscription_capacity)
    : capacity_(subscription_capacity)
{}

absl::StatusOr<std::shared_ptr<AcquisitionFeed::Subscription>> AcquisitionFeed::subscribe(const AcquisitionLedger& ledger)
{
    auto subscription = std::make_shared<Subscription>(capacity_);
    {
        std::lock_guard lock(mutex_);
        subscriptions_.push_back(subscription);
    }
    auto snapshot = ledger.snapshotAcquisitions();
    if(!snapshot.ok())
        return snapshot.status();
    subscription->seed(*snapshot);
    return subscription;
}

void AcquisitionFeed::onAcquisitionChange(const AcquisitionChange& change)
{
    std::vector<std::shared_ptr<Subscription>> live;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(subscriptions_, [&](const std::weak_ptr<Subscription>& w) {
            auto strong = w.lock();
            if(!strong)
                return true;
            live.push_back(std::move(strong));
            return false;
        });
    }
    for(auto& subscription: live)
        subscription->push(change);
}

} // namespace chronolog::visor
