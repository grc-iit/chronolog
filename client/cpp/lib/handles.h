#pragma once
#include "internal.h"

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
    std::shared_ptr<grpc::ClientContext> context;
    std::atomic<bool> cancelled{};
    bool done{};
    void cancel()
    {
        cancelled = true;
        std::lock_guard lock(cancel_mutex);
        if(context)
            context->TryCancel();
    }
    std::shared_ptr<grpc::ClientContext> newContext(TimePoint end)
    {
        auto result = std::make_shared<grpc::ClientContext>();
        result->set_deadline(overall ? std::min(*overall, end) : end);
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
    Impl(std::shared_ptr<detail::State> s, std::string endpoint, StoryId id, HlcRange r, Deadline d)
        : ReplayState(std::move(s), std::move(endpoint), id, d)
        , range(r)
    {}
    HlcRange range;
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
    size_t retries{};
    absl::StatusOr<std::optional<StreamItem>> next(Deadline);
};
} // namespace chronolog::client
