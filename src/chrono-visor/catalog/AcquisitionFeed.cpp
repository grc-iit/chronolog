#include "catalog/AcquisitionFeed.h"

#include <algorithm>
#include <utility>

namespace chronolog::visor
{

bool AcquisitionFeed::Subscription::matches(const AcquisitionChange& change) const
{
    return keeper_id_.empty() || change.assigned_keeper.process_id == keeper_id_;
}

AcquisitionSnapshot AcquisitionFeed::Subscription::snapshot() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}

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
    if(!matches(change))
        return;
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
    snapshot_.revision = snapshot.revision;
    snapshot_.active.clear();
    for(const auto& change: snapshot.active)
        if(matches(change))
            snapshot_.active.push_back(change);
}

AcquisitionFeed::AcquisitionFeed(size_t subscription_capacity)
    : capacity_(subscription_capacity)
{}

absl::StatusOr<std::shared_ptr<AcquisitionFeed::Subscription>>
AcquisitionFeed::subscribe(const AcquisitionLedger& ledger, const std::string& keeper_id)
{
    auto subscription = std::make_shared<Subscription>(capacity_, keeper_id);
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
        std::erase_if(subscriptions_,
                      [&](const std::weak_ptr<Subscription>& w)
                      {
                          auto strong = w.lock();
                          if(!strong)
                              return true;
                          live.push_back(std::move(strong));
                          return false;
                      });
    }
    for(auto& subscription: live) subscription->push(change);
}

} // namespace chronolog::visor
