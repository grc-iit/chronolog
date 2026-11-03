#ifndef STORYTELLER_CLIENT_H
#define STORYTELLER_CLIENT_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <thallium.hpp>

#include <chrono_monitor.h>
#include <ServiceId.h>
#include <chronolog_types.h>
#include <chronolog_client.h>
#include "ChronologTimer.h"
#include "RpcPolicyContext.h"
#include "ClientRpcContext.h"

namespace chronolog
{


class KeeperRecordingClient;
class PlaybackQueryRpcClient;

class RoundRobinKeeperChoice
{
public:
    KeeperRecordingClient* chooseKeeper(std::vector<KeeperRecordingClient*> const& vectorOfKeepers,
                                        uint64_t chrono_tick)
    {
        return vectorOfKeepers[chrono_tick % vectorOfKeepers.size()];
    }
};


class StorytellerClient
{
public:
    StorytellerClient(
            ChronologTimer& chronolog_timer,
            thallium::engine& client_tl_engine,
            ClientId const& client_id,
            std::shared_ptr<RpcPolicyContext> policy_context = std::make_shared<RpcPolicyContext>(RpcTimeoutPolicy{}),
            chrono_index initial_index = 0,
            std::weak_ptr<ClientRpcContext> context = {},
            std::shared_ptr<ClientRpcProcedures> procedures = {})
        : theTimer(chronolog_timer)
        , client_engine(client_tl_engine)
        , clientId(client_id)
        , atomic_index(initial_index)
        , policyContext(std::move(policy_context))
        , context_(std::move(context))
        , procedures_(std::move(procedures))
    {
        LOG_DEBUG("[StorytellerClient] Initialized with ClientID: {}", clientId);
    }

    ~StorytellerClient();

    int addKeeperRecordingClient(ServiceId const&);
    int removeKeeperRecordingClient(ServiceId const&);

    StoryHandle* findStoryHandle(ChronicleName const&, StoryName const&);

    StoryHandle* initializeStoryHandle(ChronicleName const&,
                                       StoryName const&,
                                       StoryId const&,
                                       std::vector<ServiceId> const&,
                                       ServiceId const&);

    void removeAcquiredStoryHandle(ChronicleName const&, StoryName const&);
    void clearAcquiredStoryHandles();
    size_t retainedHandleCount();
    void quiesceNativeEntries();
    std::weak_ptr<ClientRpcContext> context() const noexcept { return context_; }

    TimestampResult getTimestamp(std::chrono::steady_clock::time_point deadline) noexcept
    {
        return theTimer.next(deadline);
    }

    std::optional<RpcPolicyContext::Snapshot> acquirePolicy() { return policyContext->tryAcquire(); }

    ClientId const& getClientId() const { return clientId; }

    chrono_index get_event_index() noexcept;

    //   ServiceId const& get_local_service_id() const    { return theClientQueryService.get_service_id(); }

private:
    StorytellerClient(StorytellerClient const&) = delete;

    StorytellerClient& operator=(StorytellerClient const&) = delete;

    ChronologTimer& theTimer;
    thallium::engine& client_engine;
    ClientId clientId;
    std::atomic<chrono_index> atomic_index{0};
    std::shared_ptr<RpcPolicyContext> policyContext;
    std::weak_ptr<ClientRpcContext> context_;
    std::shared_ptr<ClientRpcProcedures> procedures_;

    std::mutex recordingClientMapMutex;
    std::mutex acquiredStoryMapMutex;

    std::map<std::pair<uint32_t, uint16_t>, KeeperRecordingClient*> recordingClientMap;
    std::map<std::pair<std::string, std::string>, StoryHandle*> acquiredStoryHandles;
    std::vector<std::unique_ptr<StoryHandle>> ownedHandles_;
    void reapRetiredHandles();
};


// this class definition lives in the client lib
// Concrete StoryHandle implementation serving both paths: log_event (write) and
// playback (keeper tail read). Templated on the write-path keeper-choice policy.
template <class KeeperChoicePolicy>
class StoryHandleImpl: public StoryHandle
{
public:
    StoryHandleImpl(StorytellerClient& client,
                    ChronicleName const& a_chronicle,
                    StoryName const& a_story,
                    StoryId const& story_id)
        : theClient(client)
        , context_(client.context())
        , retainedContext_(!context_.expired())
        , chronicle(a_chronicle)
        , story(a_story)
        , storyId(story_id)
        , keeperChoicePolicy(new KeeperChoicePolicy)
    //   , playbackQueryClient(nullptr)
    {
        LOG_DEBUG("[StoryHandleImpl] Initialized for Chronicle: {}, Story: {}", a_chronicle, a_story);
    }

    virtual ~StoryHandleImpl();

    virtual uint64_t log_event(std::string const&) override;
    virtual AppendResult append_event(std::string const&) override;

    virtual int playback(size_t n, std::vector<Event>& events) override;

    // virtual int log_event(size_t size, void*data);

    void addRecordingClient(KeeperRecordingClient*);
    // Counts admitted calls/owned continuations, not raw-pointer lifetime.
    // Callers must keep the raw object valid before and throughout entry.
    void invalidate() noexcept { entries_.fetch_or(Closed, std::memory_order_acq_rel); }
    bool retiredAndIdle() const noexcept { return entries_.load(std::memory_order_acquire) == Closed; }
    class EntryLease
    {
    public:
        EntryLease(EntryLease const&) = delete;
        EntryLease(EntryLease&& other) noexcept
            : handle_(std::exchange(other.handle_, nullptr))
            , native_(std::move(other.native_))
        {}
        ~EntryLease()
        {
            if(handle_)
                handle_->entries_.fetch_sub(1, std::memory_order_acq_rel);
            // Native lease releases AFTER the handle's final access above.
        }
        void retainNative(ClientRpcContext::NativeLease lease) { native_.emplace(std::move(lease)); }

    private:
        friend class StoryHandleImpl;
        explicit EntryLease(StoryHandleImpl* handle)
            : handle_(handle)
        {}
        StoryHandleImpl* handle_;
        std::optional<ClientRpcContext::NativeLease> native_;
    };
    // This gate cannot protect virtual dispatch or access before admission.
    std::optional<EntryLease> tryEnter() noexcept
    {
        auto observed = entries_.load(std::memory_order_acquire);
        while(!(observed & Closed) && observed != Closed - 1)
            if(entries_.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel))
                return EntryLease(this);
        return std::nullopt;
    }
    void removeRecordingClient(ServiceId const&);

private:
    friend struct StoryHandleTestAccess;
    class AppendWork;
    AppendResult append(std::string const&,
                        rpc::RpcDeadline const&,
                        RpcPolicyContext::Snapshot,
                        EntryLease,
                        std::shared_ptr<ClientRpcContext>);
    AppendResult appendFromEntry(std::string const&, rpc::RpcDeadline::Clock::time_point);
    StorytellerClient& theClient;
    std::weak_ptr<ClientRpcContext> context_;
    bool retainedContext_;
    static constexpr unsigned Closed = 1u << 31;
    std::atomic<unsigned> entries_{0};
    ChronicleName chronicle;
    StoryName story;
    StoryId storyId;
    KeeperChoicePolicy* keeperChoicePolicy;
    std::vector<KeeperRecordingClient*> storyKeepers;
};

} // namespace chronolog

#endif
