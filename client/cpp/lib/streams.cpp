#include "handles.h"

namespace chronolog::client
{
namespace
{
absl::Status cancelled() { return absl::CancelledError("stream cancelled"); }
absl::Status expired() { return absl::DeadlineExceededError("stream pull deadline"); }
template <class Response>
absl::StatusOr<StreamItem> decode(const Response& response, detail::State& state)
{
    StreamItem item;
    if(response.has_batch())
    {
        for(const auto& raw: response.batch().events())
        {
            auto event = detail::decode(raw);
            state.observe(event.hlc);
            item.events.push_back(std::move(event));
        }
    }
    else if(response.has_completion())
    {
        item.completion = detail::decode(response.completion());
        if(item.completion->reason == IncompleteReason::Truncated)
            item.continuation = item.completion->frontier;
    }
    else
        return absl::DataLossError("unset replay response");
    return item;
}
bool after(const Event& event, const Position& p, bool prefix)
{
    if(p.id == EventId{} || (p.id.writer_id == 0 && p.id.incarnation == 0 && p.id.sequence == 0))
        return event.hlc >= p.hlc;
    Event previous;
    previous.hlc = p.hlc;
    previous.id = p.id;
    return prefix ? PrefixLess(previous, event) : ReplayLess(previous, event);
}
} // namespace
ReadStream::ReadStream(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}
ReadStream::~ReadStream()
{
    if(impl_)
        impl_->cancel();
}
ReadStream::ReadStream(ReadStream&&) noexcept = default;
ReadStream& ReadStream::operator=(ReadStream&& other) noexcept
{
    if(this != &other)
    {
        if(impl_)
            impl_->cancel();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
void ReadStream::cancel()
{
    if(impl_)
        impl_->cancel();
}
absl::StatusOr<std::optional<StreamItem>> ReadStream::next(Deadline d) { return impl_->next(d); }
absl::StatusOr<std::optional<StreamItem>> ReadStream::Impl::next(Deadline deadline)
{
    if(physical)
        return nextPhysical(deadline);
    const auto end = pullEnd(deadline);
    std::unique_lock lock(pull_mutex, std::defer_lock);
    if(!lock.try_lock_until(end))
        return expired();
    if(cancelled)
        return client::cancelled();
    if(done)
        return std::optional<StreamItem>{};
    if(std::chrono::system_clock::now() >= end)
        return expired();
    if(!stream)
        newContext();
    detail::PullDeadline watchdog(context, end);
    if(!stream)
    {
        v1::ReadRequest request;
        if(prefix.empty())
            request.set_story_id(story);
        else
            request.set_prefix(prefix);
        request.set_max_events(options.max_events.value_or(0));
        if(!detail::unconstrained(options.predicate))
            detail::encode(options.predicate, request.mutable_predicate());
        detail::encode(range.start, request.mutable_hlc()->mutable_start());
        detail::encode(range.end, request.mutable_hlc()->mutable_end());
        stream = replay->Read(context.get(), request);
    }
    v1::ReadResponse response;
    if(!stream->Read(&response))
    {
        done = true;
        auto status = detail::status(stream->Finish());
        if(cancelled)
            return client::cancelled();
        if(watchdog.expired())
            return expired();
        return status.ok() ? absl::DataLossError("Read ended without Completion") : status;
    }
    auto item = client::decode(response, *state);
    if(!item.ok())
    {
        cancel();
        return item.status();
    }
    if(item->completion)
    {
        done = true;
        if(stream->Read(&response))
        {
            cancel();
            return absl::DataLossError("Read data after Completion");
        }
        auto status = detail::status(stream->Finish());
        if(cancelled)
            return client::cancelled();
        if(watchdog.expired())
            return expired();
        if(!status.ok())
            return status;
    }
    return std::optional<StreamItem>(std::move(*item));
}
absl::StatusOr<std::optional<StreamItem>> ReadStream::Impl::nextPhysical(Deadline deadline)
{
    const auto end = pullEnd(deadline);
    std::unique_lock lock(pull_mutex, std::defer_lock);
    if(!lock.try_lock_until(end))
        return expired();
    if(cancelled)
        return client::cancelled();
    if(done)
        return std::optional<StreamItem>{};
    while(!pending_ranges.empty())
    {
        if(std::chrono::system_clock::now() >= end)
            return expired();
        const auto current = pending_ranges.back();
        if(!stream)
            newContext(end);
        detail::PullDeadline watchdog(context, end);
        if(!stream)
        {
            v1::ReadRequest request;
            request.set_story_id(story);
            request.mutable_physical()->set_start_ns(current.start_ns);
            request.mutable_physical()->set_end_ns(current.end_ns);
            stream = replay->Read(context.get(), request);
        }
        v1::ReadResponse response;
        std::optional<Completion> completion;
        while(stream->Read(&response))
        {
            if(completion)
                return absl::DataLossError("Read data after Completion");
            auto item = client::decode(response, *state);
            if(!item.ok())
                return item.status();
            for(auto& event: item->events) leaf_events.push_back(std::move(event));
            if(item->completion)
                completion = *item->completion;
        }
        auto status = detail::status(stream->Finish());
        stream.reset();
        if(cancelled)
            return client::cancelled();
        if(watchdog.expired())
            return expired();
        if(!status.ok())
            return status;
        if(!completion)
            return absl::DataLossError("Read ended without Completion");
        pending_ranges.pop_back();
        const int64_t middle = static_cast<int64_t>(static_cast<__int128_t>(current.start_ns) +
                                                    (static_cast<__int128_t>(current.end_ns) - current.start_ns) / 2);
        if(completion->reason == IncompleteReason::Truncated && middle > current.start_ns)
        {
            leaf_events.clear();
            pending_ranges.push_back({middle, current.end_ns});
            pending_ranges.push_back({current.start_ns, middle});
            continue;
        }
        aggregate.complete &= completion->complete;
        auto rank = [](IncompleteReason reason)
        {
            switch(reason)
            {
                case IncompleteReason::SourceFailed:
                    return 4;
                case IncompleteReason::Truncated:
                    return 3;
                case IncompleteReason::PhysicalAxisUnbounded:
                    return 2;
                case IncompleteReason::LaggingWriters:
                    return 1;
                default:
                    return 0;
            }
        };
        if(rank(completion->reason) > rank(aggregate.reason))
            aggregate.reason = completion->reason;
        aggregate.laggards.insert(aggregate.laggards.end(), completion->laggards.begin(), completion->laggards.end());
        aggregate_frontier =
                aggregate_frontier ? std::min(*aggregate_frontier, completion->frontier) : completion->frontier;
        aggregate.frontier = *aggregate_frontier;
        StreamItem result;
        for(auto& event: leaf_events)
            if(seen.insert(event.id).second)
                result.events.push_back(std::move(event));
        leaf_events.clear();
        if(!result.events.empty())
            return std::optional<StreamItem>(std::move(result));
    }
    done = true;
    StreamItem result;
    result.completion = aggregate;
    return std::optional<StreamItem>(std::move(result));
}
TailStream::TailStream(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{}
TailStream::~TailStream()
{
    if(impl_)
        impl_->cancel();
}
TailStream::TailStream(TailStream&&) noexcept = default;
TailStream& TailStream::operator=(TailStream&& other) noexcept
{
    if(this != &other)
    {
        if(impl_)
            impl_->cancel();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
void TailStream::cancel()
{
    if(impl_)
        impl_->cancel();
}
absl::StatusOr<std::optional<StreamItem>> TailStream::next(Deadline d) { return impl_->next(d); }
absl::StatusOr<std::optional<StreamItem>> TailStream::Impl::next(Deadline deadline)
{
    const auto end = pullEnd(deadline);
    std::unique_lock lock(pull_mutex, std::defer_lock);
    if(!lock.try_lock_until(end))
        return expired();
    if(done)
        return std::optional<StreamItem>{};
    for(;;)
    {
        if(cancelled)
            return client::cancelled();
        if(std::chrono::system_clock::now() >= end)
            return expired();
        if(!stream)
            newContext();
        detail::PullDeadline watchdog(context, end);
        if(!stream)
        {
            v1::TailRequest request;
            if(prefix.empty())
                request.set_story_id(story);
            else
                request.set_prefix(prefix);
            request.set_progress(true);
            if(!detail::unconstrained(options.predicate))
                detail::encode(options.predicate, request.mutable_predicate());
            auto p = position.value_or(Position{{}, {story, 0, 0, 0}});
            detail::encode(p.hlc, request.mutable_from()->mutable_hlc());
            if(p.id != EventId{})
                detail::encode(p.id, request.mutable_from()->mutable_id());
            stream = replay->Tail(context.get(), request);
        }
        v1::TailResponse response;
        if(!stream->Read(&response))
        {
            auto status = detail::status(stream->Finish());
            stream.reset();
            if(cancelled)
                return client::cancelled();
            if(watchdog.expired() || std::chrono::system_clock::now() >= end)
                return expired();
            if(status.ok())
            {
                done = true;
                return absl::DataLossError("Tail ended without Completion");
            }
            if(!detail::retryable(status) || retries++ >= state->options.retry.max_retries)
            {
                done = true;
                return status;
            }
            backoff(std::min(end, std::chrono::system_clock::now() + state->options.retry.backoff));
            continue;
        }
        if(response.has_progress())
        {
            const auto& progress = response.progress().position();
            const Hlc frontier = detail::decode(progress.hlc());
            if(!position || frontier > position->hlc)
                position = Position{frontier, {}};
            continue;
        }
        auto item = client::decode(response, *state);
        if(!item.ok())
        {
            cancel();
            return item.status();
        }
        if(item->completion)
        {
            done = true;
            if(item->completion->complete)
            {
                cancel();
                return absl::DataLossError("Tail claimed completeness");
            }
            if(stream->Read(&response))
            {
                cancel();
                return absl::DataLossError("Tail data after Completion");
            }
            auto status = detail::status(stream->Finish());
            if(cancelled)
                return client::cancelled();
            if(watchdog.expired())
                return expired();
            if(!status.ok())
                return status;
            return std::optional<StreamItem>(std::move(*item));
        }
        if(position)
            std::erase_if(item->events, [&](const Event& e) { return !after(e, *position, !prefix.empty()); });
        if(item->events.empty())
            continue;
        const auto& last = item->events.back();
        position = Position{last.hlc, last.id};
        retries = 0;
        return std::optional<StreamItem>(std::move(*item));
    }
}
} // namespace chronolog::client
