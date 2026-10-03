#include "membership/RouteWatcher.h"

#include <absl/log/log.h>
#include "adapter/Convert.h"

namespace chronolog::keeper
{

namespace iv1 = chronolog::internal::v1;

namespace
{
constexpr int kReconcileAttempts = 6;
constexpr std::chrono::milliseconds kReconcileBackoff{100};
} // namespace

RouteWatcher::RouteWatcher(ConfigMembership& membership,
                           std::shared_ptr<grpc::Channel> channel,
                           std::string process_id,
                           std::string instance,
                           RamJournal* journal,
                           TombstoneLookup lookup)
    : journal_(journal)
    , membership_(membership)
    , stub_(iv1::Cluster::NewStub(std::move(channel)))
    , process_id_(std::move(process_id))
    , instance_(std::move(instance))
    , lookup_(std::move(lookup))
{
    if(journal_ && lookup_)
        reconciler_ = std::jthread([this](std::stop_token stop) { reconcile(stop); });
    watcher_ = std::make_unique<Watcher>([this](std::stop_token stop) { return session(stop); });
}

RouteWatcher::~RouteWatcher()
{
    watcher_.reset();
    reconciler_.request_stop();
    cv_.notify_all();
}

bool RouteWatcher::session(std::stop_token stop)
{
    grpc::ClientContext context;
    std::stop_callback cancel(stop, [&context] { context.TryCancel(); });
    iv1::WatchRoutesRequest request;
    request.set_process_id(process_id_);
    request.set_instance(instance_);
    auto reader = stub_->WatchRoutes(&context, request);
    {
        std::lock_guard lock(mu_);
        seen_.clear();
        pending_.clear();
        ++connection_;
    }
    cv_.notify_all();
    const uint64_t floor = applied_revision_;
    iv1::WatchRoutesResponse message;
    bool marked = false;
    while(reader->Read(&message))
    {
        if(message.snapshot_end())
        {
            if(!marked && journal_)
                conclude(message.revision(), floor);
            marked = true;
            continue;
        }
        // A tombstone is applied before the epoch and revision guards, whatever its revision, and never moves the
        // applied revision. Every later update for the story is ignored (I3.5).
        if(message.tombstoned())
        {
            tombstoned_.insert(message.story_id());
            if(journal_)
                (void)journal_->dropStory(message.story_id(), true);
            continue;
        }
        if(tombstoned_.contains(message.story_id()))
            continue;
        {
            std::lock_guard lock(mu_);
            seen_.insert(message.story_id());
        }
        auto current = membership_.route(message.story_id());
        if(current.ok() && message.route().epoch() < current->epoch)
            continue;
        auto& prior = applied_[message.story_id()];
        if(message.revision() < applied_revision_ || message.revision() < prior.first ||
           message.route().epoch() < prior.second)
            continue;
        applied_revision_ = message.revision();
        prior = {message.revision(), message.route().epoch()};
        learned_.try_emplace(message.story_id(), message.revision());
        auto state = convert::routeState(message);
        auto install = [&] { membership_.setRouteState(message.story_id(), state); };
        if(journal_)
            journal_->applyRoute(message.story_id(),
                                 state,
                                 std::find(message.observe_floor().begin(),
                                           message.observe_floor().end(),
                                           process_id_) != message.observe_floor().end(),
                                 message.revision(),
                                 install);
        else
            install();
    }
    reader->Finish();
    // A stream that ended before its marker is a failed connect, retried with the backoff.
    return marked;
}

void RouteWatcher::conclude(uint64_t revision, uint64_t floor)
{
    // The snapshot lists every live story at R and story ids are never reused (W10.17, I3.5), so an unlisted story
    // this watcher saw live at or below R is destroyed. A snapshot below the revision already applied concludes
    // nothing.
    std::set<StoryId> absent;
    {
        std::lock_guard lock(mu_);
        for(auto story: journal_->storyIds())
            if(!seen_.contains(story) && !journal_->dropped(story))
                absent.insert(story);
        for(auto story: journal_->droppedUnconfirmed())
            if(!seen_.contains(story))
                absent.insert(story);
    }
    std::set<StoryId> pending;
    for(auto story: absent)
    {
        auto learned = learned_.find(story);
        if(revision < floor || learned == learned_.end() || learned->second > revision)
        {
            pending.insert(story);
            continue;
        }
        tombstoned_.insert(story);
        (void)journal_->dropStory(story, true);
    }
    {
        std::lock_guard lock(mu_);
        pending_ = std::move(pending);
        marked_ = connection_;
    }
    cv_.notify_all();
}

void RouteWatcher::reconcile(std::stop_token stop)
{
    while(!stop.stop_requested())
    {
        uint64_t connection;
        std::set<StoryId> pending;
        {
            std::unique_lock lock(mu_);
            if(!cv_.wait(lock, stop, [this] { return marked_ != reconciled_; }))
                return;
            connection = reconciled_ = marked_;
            pending = std::move(pending_);
            pending_.clear();
        }
        for(auto story: pending)
        {
            auto backoff = kReconcileBackoff;
            for(int attempt = 0; attempt < kReconcileAttempts && !stop.stop_requested(); ++attempt)
            {
                {
                    std::lock_guard lock(mu_);
                    if(connection_ != connection || seen_.contains(story))
                        break;
                }
                auto tombstone = lookup_(story);
                if(tombstone.ok())
                {
                    if(*tombstone)
                        (void)journal_->dropStory(story, true);
                    break;
                }
                std::unique_lock lock(mu_);
                cv_.wait_for(lock, stop, backoff, [] { return false; });
                backoff *= 2;
            }
        }
    }
}

} // namespace chronolog::keeper
