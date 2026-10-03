#include "internal.h"
#include <condition_variable>
#include <thread>

namespace chronolog::context
{
absl::StatusOr<FollowResult>
ContextClient::follow(std::span<const FollowInput> inputs, FollowOptions options, Deadline deadline)
{
    if(inputs.empty() || !options.limits.max_events || !options.limits.max_raw_bytes || options.wait.count() < 0 ||
       options.wait > std::chrono::seconds(60))
        return absl::InvalidArgumentError("invalid follow inputs or limits");
    deadline = detail::deadline(impl_->options, deadline);
    for(const auto& input: inputs)
        if(!input.session || input.session->impl_->core != impl_)
            return absl::InvalidArgumentError("follow session belongs to another ContextClient");
    FollowResult result;
    result.pages.resize(inputs.size());
    std::vector<std::optional<client::TailStream>> streams(inputs.size());
    std::vector<std::unique_ptr<detail::IoRegistration>> registrations(inputs.size());
    for(size_t i = 0; i < inputs.size(); ++i)
    {
        const auto& input = inputs[i];
        auto& output = result.pages[i];
        output.context = input.session->context();
        output.page.idle = true;
        const StoryId story = output.context.story_id;
        Position start{{}, {story, 0, 0, 0}};
        absl::Status failure;
        if(input.from == FollowFrom::Position)
        {
            if(!input.after || !detail::validPosition(*input.after, story, true))
                return absl::InvalidArgumentError("invalid follow Position");
            start = *input.after;
        }
        else if(input.from == FollowFrom::Now)
        {
            const Hlc candidate = std::max(impl_->sdk.causalFloor(), detail::realtime());
            auto route = impl_->sdk.route(story, deadline);
            if(route.ok())
                for(const auto& keeper: route->keepers) output.uncertified_route_keepers.push_back(keeper.process_id);
            if(!route.ok())
                failure = route.status();
            else if(candidate > Hlc{})
            {
                auto range = detail::probeRange(candidate, impl_->options.cut_probe_width);
                auto probe = detail::readPage(impl_->sdk,
                                              story,
                                              range,
                                              options.limits,
                                              deadline,
                                              input.session->impl_->record);
                auto current = impl_->sdk.route(story, deadline);
                if(current.ok())
                {
                    for(const auto& keeper: current->keepers)
                        output.uncertified_route_keepers.push_back(keeper.process_id);
                    std::sort(output.uncertified_route_keepers.begin(), output.uncertified_route_keepers.end());
                    auto end = std::unique(output.uncertified_route_keepers.begin(),
                                           output.uncertified_route_keepers.end());
                    output.uncertified_route_keepers.erase(end, output.uncertified_route_keepers.end());
                }
                if(!current.ok())
                    failure = current.status();
                else if(!probe.stream_status.ok())
                    failure = probe.stream_status;
                else if(!probe.completion || probe.completion->reason == IncompleteReason::SourceFailed)
                    failure = absl::UnavailableError("follow(now) Route is not fully certified");
                else
                    start.hlc = std::min(candidate, probe.completion->frontier);
                if(!failure.ok())
                {
                    output.page.completion = probe.completion;
                    output.page.completion_range = probe.completion_range;
                }
            }
            if(failure.ok())
            {
                output.starting_cut = start.hlc;
                output.uncertified_route_keepers.clear();
            }
        }
        else if(input.from != FollowFrom::Beginning)
            return absl::InvalidArgumentError("invalid follow start kind");
        if(!failure.ok())
        {
            output.page.stream_status = failure;
            output.page.idle = false;
            if(result.status.ok())
                result.status = failure;
            continue;
        }
        output.resume = start;
        auto tail = impl_->sdk.tail(story, start, deadline);
        if(!tail.ok())
        {
            output.page.stream_status = tail.status();
            output.page.idle = false;
            if(result.status.ok())
                result.status = tail.status();
            continue;
        }
        streams[i] = std::move(*tail);
        registrations[i] = std::make_unique<detail::IoRegistration>(input.session->impl_->record,
                                                                    [&, i] { streams[i]->cancel(); });
    }
    std::mutex mutex;
    std::condition_variable ready;
    bool finished = std::none_of(streams.begin(), streams.end(), [](const auto& stream) { return stream.has_value(); });
    bool stopping = false;
    std::vector<std::thread> workers;
    for(size_t i = 0; i < streams.size(); ++i)
    {
        if(!streams[i])
            continue;
        workers.emplace_back(
                [&, i]
                {
                    auto item = streams[i]->next(deadline);
                    std::lock_guard lock(mutex);
                    auto& output = result.pages[i];
                    if(!item.ok())
                    {
                        if(!(stopping && item.status().code() == absl::StatusCode::kCancelled))
                        {
                            output.page.stream_status = item.status();
                            output.page.idle = false;
                            if(result.status.ok())
                                result.status = item.status();
                            finished = true;
                        }
                    }
                    else if(*item)
                    {
                        auto& batch = **item;
                        output.page.completion = batch.completion;
                        for(auto& event: batch.events)
                        {
                            const size_t bytes = detail::rawBytes(event.envelope);
                            if(!output.page.events.empty() &&
                               (output.page.events.size() >= options.limits.max_events ||
                                bytes > options.limits.max_raw_bytes -
                                                std::min(output.page.raw_bytes, options.limits.max_raw_bytes)))
                            {
                                output.page.limited = output.page.events.size() >= options.limits.max_events
                                                              ? DeliveryLimit::Events
                                                              : DeliveryLimit::Bytes;
                                break;
                            }
                            output.page.raw_bytes += bytes;
                            if(output.page.raw_bytes > options.limits.max_raw_bytes)
                                output.page.limited = DeliveryLimit::OversizedEvent;
                            output.resume = Position{event.hlc, event.id};
                            output.page.after = output.resume;
                            output.page.events.push_back(std::move(event));
                        }
                        output.page.idle = output.page.events.empty() && !batch.completion;
                        if(!output.page.idle)
                            finished = true;
                    }
                    ready.notify_all();
                });
    }
    const auto idle_end = std::chrono::system_clock::now() + options.wait;
    {
        std::unique_lock lock(mutex);
        if(!ready.wait_until(lock, std::min(idle_end, *deadline), [&] { return finished; }) &&
           std::chrono::system_clock::now() >= *deadline && result.status.ok())
            result.status = absl::DeadlineExceededError("follow caller deadline");
        stopping = true;
    }
    for(auto& stream: streams)
        if(stream)
            stream->cancel();
    for(auto& worker: workers) worker.join();
    result.idle = result.status.ok() && std::all_of(result.pages.begin(),
                                                    result.pages.end(),
                                                    [](const FollowPage& page) { return page.page.idle; });
    return result;
}
} // namespace chronolog::context
