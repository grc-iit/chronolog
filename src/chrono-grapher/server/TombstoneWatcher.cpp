#include "chrono-grapher/server/TombstoneWatcher.h"
#include <absl/log/log.h>
#include <algorithm>

namespace chronolog::grapher
{
namespace
{
constexpr int kReconcileAttempts = 6;
constexpr std::chrono::milliseconds kReconcileBackoff{100};
constexpr std::chrono::milliseconds kReconnectFloor{100}, kReconnectCap{2000};
} // namespace

TombstoneWatcher::TombstoneWatcher(ArchiveService& archive,
                                   std::shared_ptr<grpc::Channel> channel,
                                   std::string process_id,
                                   std::string instance,
                                   TombstoneLookup lookup)
    : archive_(archive)
    , stub_(internal::v1::Cluster::NewStub(std::move(channel)))
    , process_id_(std::move(process_id))
    , instance_(std::move(instance))
    , lookup_(std::move(lookup))
{
    reconciler_ = std::jthread([this](std::stop_token stop) { reconcile(stop); });
    watcher_ = std::jthread([this](std::stop_token stop) { watch(stop); });
}

TombstoneWatcher::~TombstoneWatcher()
{
    watcher_.request_stop();
    watcher_.join();
    reconciler_.request_stop();
    cv_.notify_all();
}

void TombstoneWatcher::watch(std::stop_token stop)
{
    auto delay = kReconnectFloor;
    std::mutex mutex;
    std::condition_variable_any pause;
    while(!stop.stop_requested())
    {
        if(session(stop))
            delay = kReconnectFloor;
        std::unique_lock lock(mutex);
        pause.wait_for(lock, stop, delay, [] { return false; });
        delay = std::min(delay * 2, kReconnectCap);
    }
}

bool TombstoneWatcher::session(std::stop_token stop)
{
    grpc::ClientContext context;
    std::stop_callback cancel(stop, [&context] { context.TryCancel(); });
    internal::v1::WatchRoutesRequest request;
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
    internal::v1::WatchRoutesResponse message;
    const auto floor = revision_;
    bool marked = false;
    while(reader->Read(&message))
    {
        if(message.snapshot_end())
        {
            if(!marked)
                conclude(message.revision(), floor);
            marked = true;
            continue;
        }
        const auto story = message.story_id();
        if(message.tombstoned())
        {
            tombstoned_.insert(story);
            archive_.tombstone(story);
            revision_ = std::max(revision_, message.revision());
            continue;
        }
        if(tombstoned_.contains(story))
            continue;
        {
            std::lock_guard lock(mu_);
            seen_.insert(story);
        }
        if(message.revision() < revision_ || message.route().epoch() < epochs_[story])
            continue;
        revision_ = message.revision();
        epochs_[story] = message.route().epoch();
        learned_.try_emplace(story, message.revision());
    }
    reader->Finish();
    return marked;
}

void TombstoneWatcher::conclude(uint64_t revision, uint64_t floor)
{
    std::set<StoryId> absent, pending;
    {
        const auto held = archive_.storiesToConfirm();
        std::lock_guard lock(mu_);
        for(auto story: held)
            if(!seen_.contains(story))
                absent.insert(story);
    }
    for(auto story: absent)
    {
        const auto learned = learned_.find(story);
        if(revision < floor || learned == learned_.end() || learned->second > revision)
            pending.insert(story);
        else
        {
            tombstoned_.insert(story);
            archive_.tombstone(story);
        }
    }
    revision_ = std::max(revision_, revision);
    {
        std::lock_guard lock(mu_);
        pending_ = std::move(pending);
        marked_ = connection_;
    }
    cv_.notify_all();
}

void TombstoneWatcher::reconcile(std::stop_token stop)
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
        for(const auto story: pending)
        {
            auto backoff = kReconcileBackoff;
            for(int attempt = 0; attempt < kReconcileAttempts && !stop.stop_requested(); ++attempt)
            {
                {
                    std::lock_guard lock(mu_);
                    if(connection_ != connection || seen_.contains(story))
                        break;
                }
                const auto tombstoned = lookup_(story);
                if(tombstoned.ok())
                {
                    if(*tombstoned)
                        archive_.tombstone(story);
                    break;
                }
                std::unique_lock lock(mu_);
                cv_.wait_for(lock, stop, backoff, [] { return false; });
                backoff *= 2;
            }
        }
    }
}
} // namespace chronolog::grapher
