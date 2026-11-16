#include "KeeperTailReader.h"
#include "KeeperRecordingClient.h"
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
// One keeper of the story: its sequences, written by its sequences job and read
// by the coordinator once ready is set, and the sequences chosen from it.
struct TailSource
{
    std::atomic<bool> ready{false};
    int status = CL_ERR_UNKNOWN;
    bool timedOut = false;
    std::vector<EventSequence> sequences, selection;
};
struct TailShared
{
    TailShared(std::shared_ptr<void> retained, size_t count)
        : retention(std::move(retained))
        , sources(count)
    {}
    std::shared_ptr<void> retention;
    std::vector<TailSource> sources;
};
using TailRequestPtr = std::shared_ptr<KeeperRecordingClient::TailRequest>;
using Barrier = std::shared_ptr<rpc::FanoutIssueBarrier>;
void pause(rpc::RpcDeadline const& deadline)
{
    auto duration =
            std::min(deadline.remaining(),
                     std::chrono::duration_cast<rpc::RpcDeadline::Clock::duration>(std::chrono::milliseconds(1)));
    if(duration > rpc::RpcDeadline::Clock::duration::zero())
        std::this_thread::sleep_for(duration);
}
// Asks one keeper for its newest sequences. The job ends once that call has, so
// it holds a worker only for its own call.
class TailSequences final: public rpc::OwnedRpcWork<int>
{
public:
    TailSequences(std::shared_ptr<TailShared> shared,
                  size_t index,
                  TailRequestPtr request,
                  Barrier issued,
                  StoryId story,
                  size_t n,
                  uint64_t cap)
        : shared_(std::move(shared))
        , index_(index)
        , request_(std::move(request))
        , issued_(std::move(issued))
        , story_(story)
        , n_(n)
        , cap_(cap)
    {}
    rpc::RpcResult<int> run(rpc::OperationContext& operation) override
    {
        auto& source = shared_->sources[index_];
        auto sequences = request_->sequences(operation, story_, n_, cap_, *issued_);
        source.status = sequences.status;
        source.timedOut = request_->timedOut();
        if(sequences.reply)
            source.sequences = std::move(*sequences.reply);
        source.ready.store(true, std::memory_order_release);
        if(!sequences.reply && sequences.may_have_dispatched)
            return {sequences.status, true, std::nullopt};
        return {CL_SUCCESS, sequences.may_have_dispatched, sequences.status};
    }

private:
    std::shared_ptr<TailShared> shared_;
    size_t index_;
    TailRequestPtr request_;
    Barrier issued_;
    StoryId story_;
    size_t n_;
    uint64_t cap_;
};
// Fetches the events of the sequences chosen from one keeper.
class TailEvents final: public rpc::OwnedRpcWork<TailReply>
{
public:
    TailEvents(std::shared_ptr<TailShared> shared,
               size_t index,
               TailRequestPtr request,
               Barrier issued,
               StoryId story,
               uint64_t cap)
        : shared_(std::move(shared))
        , index_(index)
        , request_(std::move(request))
        , issued_(std::move(issued))
        , story_(story)
        , cap_(cap)
    {}
    rpc::RpcResult<TailReply> run(rpc::OperationContext& operation) override
    {
        auto events = request_->events(operation, story_, shared_->sources[index_].selection, cap_, *issued_);
        if(!events.reply)
        {
            if(events.may_have_dispatched)
                return {events.status, true, std::nullopt};
            return {CL_SUCCESS, false, TailReply{events.status, {}}};
        }
        return {CL_SUCCESS, true, TailReply{CL_SUCCESS, std::move(*events.reply)}};
    }

private:
    std::shared_ptr<TailShared> shared_;
    size_t index_;
    TailRequestPtr request_;
    Barrier issued_;
    StoryId story_;
    uint64_t cap_;
};
// Runs one job per entry of `order` in waves of as many jobs as the runtime has
// slots (fewer for the last wave). A wave starts only once it holds all its
// slots, issues its calls together and is waited for before the next one, so a
// story can have more keepers than the runtime has slots. Without `retry`, a
// first wave that cannot get every slot at once returns at once, before any
// dispatch, as a read with no more keepers than slots always has. With it, a
// wave gives back what it holds and tries again until the deadline, for slots
// that this read's own earlier jobs are giving back. Returns CL_SUCCESS once
// every entry ran, or the reservation status that stopped it.
template <class Reply, class Make, class Done>
int runInWaves(std::vector<size_t> const& order,
               rpc::RpcRuntime& runtime,
               rpc::RpcDeadline const& deadline,
               bool retry,
               Make make,
               Done done)
{
    size_t next = 0;
    while(next < order.size())
    {
        size_t const want = std::min(runtime.capacity(), order.size() - next);
        std::vector<rpc::RpcReservation> reservations;
        while(reservations.size() < want)
        {
            auto token = runtime.reserve(deadline);
            if(token)
            {
                reservations.push_back(std::move(token));
                continue;
            }
            reservations.clear();
            if(!retry || token.status() != CL_ERR_RPC_CAPACITY || deadline.expired())
                return token.status();
            pause(deadline);
        }
        auto issued = std::make_shared<rpc::FanoutIssueBarrier>(static_cast<unsigned>(want));
        std::vector<rpc::RpcPending<Reply>> pending;
        for(size_t i = 0; i < want; ++i)
            pending.push_back(runtime.submit<Reply>(std::move(reservations[i]), make(order[next + i], issued)));
        for(size_t i = 0; i < want; ++i) done(order[next + i], pending[i].wait());
        next += want;
        retry = true;
    }
    return CL_SUCCESS;
}
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
    std::vector<TailRequestPtr> requests;
    std::vector<size_t> all;
    for(size_t i = 0; i < keepers.size(); ++i)
    {
        if(!keepers[i])
            return CL_ERR_UNKNOWN;
        requests.push_back(keepers[i]->prepareTail());
        all.push_back(i);
    }
    auto shared = std::make_shared<TailShared>(std::move(retention), keepers.size());
    size_t ran = 0;
    int stopped = runInWaves<int>(
            all,
            runtime,
            deadline,
            false,
            [&](size_t i, Barrier issued)
            { return std::make_shared<TailSequences>(shared, i, requests[i], std::move(issued), story, n, cap); },
            [&](size_t, rpc::RpcResult<int> const&) { ++ran; });
    if(ran == 0)
        return stopped;
    bool incomplete = stopped != CL_SUCCESS, sourceTimedOut = false;
    size_t answered = 0;
    std::map<EventSequence, size_t> sequences;
    for(size_t i = 0; i < keepers.size(); ++i)
    {
        auto& source = shared->sources[i];
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
    std::vector<size_t> selected;
    for(size_t i = 0; i < keepers.size(); ++i)
        if(!shared->sources[i].selection.empty())
            selected.push_back(i);
    std::map<EventSequence, Event> assembled;
    if(runInWaves<TailReply>(
               selected,
               runtime,
               deadline,
               true,
               [&](size_t i, Barrier issued)
               { return std::make_shared<TailEvents>(shared, i, requests[i], std::move(issued), story, cap); },
               [&](size_t, rpc::RpcResult<TailReply> result)
               {
                   if(!result.reply || result.reply->status != CL_SUCCESS)
                   {
                       incomplete = true;
                       return;
                   }
                   for(auto const& event: result.reply->events)
                   {
                       Event value(event.time(), event.getClientId(), event.index(), event.getRecord());
                       assembled[value.sequence()] = std::move(value);
                   }
               }) != CL_SUCCESS)
        incomplete = true;
    output.reserve(assembled.size());
    for(auto& event: assembled) output.push_back(std::move(event.second));
    if(!answered)
        return deadline.expired() || sourceTimedOut ? CL_ERR_QUERY_TIMED_OUT : CL_ERR_UNKNOWN;
    if(assembled.size() < take || incomplete)
        return output.empty() && deadline.expired() ? CL_ERR_QUERY_TIMED_OUT : CL_ERR_PARTIAL_RESULT;
    return CL_SUCCESS;
}
} // namespace chronolog
