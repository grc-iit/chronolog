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
                           TombstoneLookup lookup,
                           std::chrono::milliseconds settle)
    : journal_(journal)
    , membership_(membership)
    , stub_(iv1::Cluster::NewStub(std::move(channel)))
    , process_id_(std::move(process_id))
    , instance_(std::move(instance))
    , lookup_(std::move(lookup))
    , settle_(settle)
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
        ++connection_;
    }
    cv_.notify_all();
    iv1::WatchRoutesResponse message;
    bool progressed = false;
    while(reader->Read(&message))
    {
        // A tombstone is applied before the epoch and revision guards, whatever its revision, and never moves the
        // applied revision. Every later update for the story is ignored (I3.5).
        if(message.tombstoned())
        {
            tombstoned_.insert(message.story_id());
            if(journal_)
                (void)journal_->dropStory(message.story_id(), true);
            progressed = true;
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
        progressed = true;
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
    return progressed;
}

void RouteWatcher::reconcile(std::stop_token stop)
{
    while(!stop.stop_requested())
    {
        uint64_t connection;
        {
            std::unique_lock lock(mu_);
            if(!cv_.wait(lock, stop, [this] { return connection_ != reconciled_; }))
                return;
            connection = reconciled_ = connection_;
            // The snapshot is a burst at the start of the stream; the settle delay lets it land.
            cv_.wait_for(lock, stop, settle_, [] { return false; });
        }
        std::set<StoryId> pending;
        {
            std::lock_guard lock(mu_);
            for(auto story: journal_->storyIds())
                if(!seen_.contains(story) && !journal_->dropped(story))
                    pending.insert(story);
        }
        for(auto story: journal_->droppedUnconfirmed()) pending.insert(story);
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
