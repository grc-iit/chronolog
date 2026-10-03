#pragma once
#include "internal.h"
#include <set>

namespace chronolog::client
{
struct Writer::Impl
{
    Impl(std::shared_ptr<detail::State> s, Acquisition a)
        : state(std::move(s))
        , acquired(std::move(a))
    {}
    std::shared_ptr<detail::State> state;
    mutable std::timed_mutex mutex;
    mutable std::mutex acquisition_mutex;
    Acquisition acquired;
    uint64_t sequence{1};
    uint64_t batch_id{};
    bool requires_reacquisition{};
    std::optional<absl::Status> fenced;
    struct Pending
    {
        std::vector<AppendSpec> specs;
        std::vector<v1::AppendItem> items;
        std::vector<std::optional<absl::StatusOr<AppendResult>>> outcomes;
    };
    std::optional<Pending> pending;
    absl::StatusOr<BatchResult> append(std::span<const AppendSpec>, Deadline, bool stream);
};
namespace detail
{
struct ReplayState
{
    ReplayState(std::shared_ptr<State> s, std::string endpoint, StoryId id, Deadline d)
        : state(std::move(s))
        , replay(v1::Replay::NewStub(state->channel(endpoint)))
        , story(id)
        , overall(d)
    {}
    virtual ~ReplayState() = default;
    std::shared_ptr<State> state;
    std::unique_ptr<v1::Replay::Stub> replay;
    StoryId story;
    Deadline overall;
    std::timed_mutex pull_mutex;
    std::mutex cancel_mutex;
    std::condition_variable cancel_cv;
    std::shared_ptr<grpc::ClientContext> context;
    std::atomic<bool> cancelled{};
    bool done{};
    void cancel()
    {
        {
            std::lock_guard lock(cancel_mutex);
            cancelled = true;
            if(context)
                context->TryCancel();
        }
        cancel_cv.notify_all();
    }
    // Waits until `until`; false when cancel() interrupted the wait.
    bool backoff(TimePoint until)
    {
        std::unique_lock lock(cancel_mutex);
        return !cancel_cv.wait_until(lock, until, [&] { return cancelled.load(); });
    }
    // A stream RPC carries only the caller's overall deadline. Each next() is bounded by PullDeadline.
    std::shared_ptr<grpc::ClientContext> newContext(std::optional<TimePoint> end = {})
    {
        auto result = std::make_shared<grpc::ClientContext>();
        result->set_wait_for_ready(true);
        if(overall)
            end = end ? std::min(*overall, *end) : *overall;
        if(end)
            result->set_deadline(*end);
        std::lock_guard lock(cancel_mutex);
        context = result;
        if(cancelled)
            result->TryCancel();
        return result;
    }
    TimePoint pullEnd(Deadline d) const
    {
        auto end = state->deadline(d);
        return overall ? std::min(*overall, end) : end;
    }
};
} // namespace detail
struct ReadStream::Impl: detail::ReplayState
{
    Impl(std::shared_ptr<detail::State> s, std::string endpoint, StoryId id, HlcRange r, ReadOptions o, Deadline d)
        : ReplayState(std::move(s), std::move(endpoint), id, d)
        , range(r)
        , options(o)
    {}
    Impl(std::shared_ptr<detail::State> s, std::string endpoint, StoryId id, PhysicalRange r, Deadline d)
        : ReplayState(std::move(s), std::move(endpoint), id, d)
        , physical(r)
        , pending_ranges{r}
    {}
    HlcRange range;
    ReadOptions options;
    std::optional<PhysicalRange> physical;
    std::vector<PhysicalRange> pending_ranges;
    std::set<EventId> seen;
    Completion aggregate{true, {}, {}, IncompleteReason::None};
    std::optional<Hlc> aggregate_frontier;
    std::vector<Event> leaf_events;
    absl::StatusOr<std::optional<StreamItem>> nextPhysical(Deadline);
    std::unique_ptr<grpc::ClientReader<v1::ReadResponse>> stream;
    absl::StatusOr<std::optional<StreamItem>> next(Deadline);
};
struct TailStream::Impl: detail::ReplayState
{
    Impl(std::shared_ptr<detail::State> s, std::string endpoint, StoryId id, std::optional<Position> p, Deadline d)
        : ReplayState(std::move(s), std::move(endpoint), id, d)
        , position(p)
    {}
    std::optional<Position> position;
    std::unique_ptr<grpc::ClientReader<v1::TailResponse>> stream;
    size_t retries{}; // consecutive failures since the last delivered event
    absl::StatusOr<std::optional<StreamItem>> next(Deadline);
};
} // namespace chronolog::client
