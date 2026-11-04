#include "KeeperTailReader.h"
#include "KeeperRecordingClient.h"
#include <array>
#include <atomic>
#include <map>
#include <thread>
namespace chronolog
{
namespace
{
struct TailReply
{
    int status;
    std::vector<LogEvent> events;
};
struct TailMailbox
{
    std::atomic<bool> ready{false}, selected{false};
    int status = CL_ERR_UNKNOWN;
    bool timedOut = false;
    std::vector<EventSequence> sequences, selection;
};
struct TailShared
{
    explicit TailShared(std::shared_ptr<void> retained, unsigned count)
        : retention(std::move(retained))
        , sequenceIssued(count)
    {}
    std::shared_ptr<void> retention;
    rpc::FanoutIssueBarrier sequenceIssued;
    std::optional<rpc::FanoutIssueBarrier> payloadIssued;
    std::atomic<bool> abandoned{false};
    std::array<TailMailbox, 2> sources;
};
void pause(rpc::RpcDeadline const& deadline)
{
    auto duration =
            std::min(deadline.remaining(),
                     std::chrono::duration_cast<rpc::RpcDeadline::Clock::duration>(std::chrono::milliseconds(1)));
    if(duration > rpc::RpcDeadline::Clock::duration::zero())
        std::this_thread::sleep_for(duration);
}
class TailChild final: public rpc::OwnedRpcWork<TailReply>
{
public:
    TailChild(std::shared_ptr<TailShared> shared,
              size_t index,
              std::unique_ptr<KeeperRecordingClient::TailRequest> request,
              StoryId story,
              size_t n,
              uint64_t cap)
        : shared_(std::move(shared))
        , index_(index)
        , request_(std::move(request))
        , story_(story)
        , n_(n)
        , cap_(cap)
    {}
    rpc::RpcResult<TailReply> run(rpc::OperationContext& operation) override
    {
        auto& mailbox = shared_->sources[index_];
        auto sequences = request_->sequences(operation, story_, n_, cap_, shared_->sequenceIssued);
        mailbox.status = sequences.status;
        mailbox.timedOut = request_->timedOut();
        if(sequences.reply)
            mailbox.sequences = std::move(*sequences.reply);
        mailbox.ready.store(true, std::memory_order_release);
        if(!sequences.reply)
        {
            if(sequences.may_have_dispatched)
                return {sequences.status, true, std::nullopt};
            return {CL_SUCCESS, false, TailReply{sequences.status, {}}};
        }
        while(!mailbox.selected.load(std::memory_order_acquire))
        {
            if(shared_->abandoned.load(std::memory_order_acquire) || operation.deadline().expired())
                return {CL_SUCCESS, true, TailReply{CL_ERR_QUERY_TIMED_OUT, {}}};
            pause(operation.deadline());
        }
        if(shared_->abandoned.load(std::memory_order_acquire) || operation.deadline().expired())
            return {CL_SUCCESS, true, TailReply{CL_ERR_QUERY_TIMED_OUT, {}}};
        if(mailbox.selection.empty())
            return {CL_SUCCESS, true, TailReply{CL_SUCCESS, {}}};
        auto events = request_->events(operation, story_, mailbox.selection, cap_, *shared_->payloadIssued);
        if(!events.reply)
        {
            if(events.may_have_dispatched)
                return {events.status, true, std::nullopt};
            return {CL_SUCCESS, true, TailReply{events.status, {}}};
        }
        return {CL_SUCCESS, true, TailReply{CL_SUCCESS, std::move(*events.reply)}};
    }

private:
    std::shared_ptr<TailShared> shared_;
    size_t index_;
    std::unique_ptr<KeeperRecordingClient::TailRequest> request_;
    StoryId story_;
    size_t n_;
    uint64_t cap_;
};
} // namespace
int gather_story_tail(std::vector<KeeperRecordingClient*> const& keepers,
                      StoryId const& story,
                      size_t n,
                      std::vector<Event>& output,
                      rpc::RpcDeadline const& deadline,
                      rpc::RpcRuntime& runtime,
                      std::shared_ptr<void> retention,
                      uint64_t cap)
{
    output.clear();
    if(keepers.empty())
        return CL_ERR_NO_KEEPERS;
    if(n == 0)
        return CL_SUCCESS;
    if(keepers.size() > 2)
        return CL_ERR_RPC_CAPACITY;
    std::array<rpc::RpcReservation, 2> reservations;
    for(size_t i = 0; i < keepers.size(); ++i)
    {
        if(!keepers[i])
            return CL_ERR_UNKNOWN;
        reservations[i] = runtime.reserve(deadline);
        if(!reservations[i])
            return reservations[i].status();
    }
    auto shared = std::make_shared<TailShared>(std::move(retention), keepers.size());
    struct Abandon
    {
        std::shared_ptr<TailShared> state;
        ~Abandon() { state->abandoned.store(true, std::memory_order_release); }
    } abandon{shared};
    std::array<rpc::RpcPending<TailReply>, 2> pending;
    for(size_t i = 0; i < keepers.size(); ++i)
        pending[i] = runtime.submit<TailReply>(
                std::move(reservations[i]),
                std::make_shared<TailChild>(shared, i, keepers[i]->prepareTail(), story, n, cap));
    bool incomplete = false, sourceTimedOut = false;
    size_t answered = 0;
    std::map<EventSequence, size_t> sequences;
    for(size_t i = 0; i < keepers.size(); ++i)
    {
        auto& source = shared->sources[i];
        while(!source.ready.load(std::memory_order_acquire) && !deadline.expired()) pause(deadline);
        bool ready = source.ready.load(std::memory_order_acquire);
        if(ready)
            sourceTimedOut |= source.timedOut;
        if(!ready || source.status != CL_SUCCESS)
        {
            incomplete = true;
            continue;
        }
        ++answered;
        for(auto const& sequence: source.sequences) sequences[sequence] = i;
    }
    size_t take = std::min(n, sequences.size());
    auto sequence = sequences.end();
    for(size_t i = 0; i < take; ++i)
    {
        --sequence;
        shared->sources[sequence->second].selection.push_back(sequence->first);
    }
    unsigned selected = 0;
    for(size_t i = 0; i < keepers.size(); ++i)
        if(!shared->sources[i].selection.empty())
            ++selected;
    shared->payloadIssued.emplace(selected);
    for(size_t i = 0; i < keepers.size(); ++i) shared->sources[i].selected.store(true, std::memory_order_release);
    std::map<EventSequence, Event> assembled;
    for(size_t i = 0; i < keepers.size(); ++i)
    {
        auto result = pending[i].wait();
        if(!result.reply || result.reply->status != CL_SUCCESS)
        {
            incomplete = true;
            continue;
        }
        for(auto const& event: result.reply->events)
        {
            Event value(event.time(), event.getClientId(), event.index(), event.getRecord());
            assembled[value.sequence()] = std::move(value);
        }
    }
    output.reserve(assembled.size());
    for(auto& event: assembled) output.push_back(std::move(event.second));
    if(!answered)
        return deadline.expired() || sourceTimedOut ? CL_ERR_QUERY_TIMED_OUT : CL_ERR_UNKNOWN;
    if(assembled.size() < take || incomplete)
        return output.empty() && deadline.expired() ? CL_ERR_QUERY_TIMED_OUT : CL_ERR_PARTIAL_RESULT;
    return CL_SUCCESS;
}
} // namespace chronolog
