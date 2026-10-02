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
                                   TombstoneLookup lookup,
                                   std::chrono::milliseconds settle)
    : archive_(archive)
    , stub_(internal::v1::Cluster::NewStub(std::move(channel)))
    , process_id_(std::move(process_id))
    , instance_(std::move(instance))
    , lookup_(std::move(lookup))
    , settle_(settle)
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
        ++connection_;
    }
    cv_.notify_all();
    internal::v1::WatchRoutesResponse message;
    bool progressed = false;
    while(reader->Read(&message))
    {
        progressed = true;
        {
            std::lock_guard lock(mu_);
            seen_.insert(message.story_id());
        }
        if(message.tombstoned())
            archive_.tombstone(message.story_id());
    }
    reader->Finish();
    return progressed;
}

void TombstoneWatcher::reconcile(std::stop_token stop)
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
        std::vector<StoryId> pending;
        {
            const auto held = archive_.storiesToConfirm();
            std::lock_guard lock(mu_);
            for(const auto story: held)
                if(!seen_.contains(story))
                    pending.push_back(story);
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
