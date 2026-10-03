#include "internal.h"
#include <deque>
#include <limits>

namespace chronolog::context
{
namespace detail
{
absl::StatusOr<LatestResult> latestSearch(client::Client& sdk,
                                          const ContextOptions& config,
                                          StoryId story,
                                          size_t n,
                                          const LatestOptions& options,
                                          Deadline deadline,
                                          std::shared_ptr<Record> record,
                                          const std::function<bool(const Event&)>& match)
{
    if(!n || n > options.limits.max_events)
        return absl::InvalidArgumentError("latest n must be positive and fit the configured event limit");
    if(!options.limits.max_raw_bytes || !options.max_read_calls)
        return absl::InvalidArgumentError("latest limits must be positive");
    if(config.cut_probe_width <= std::chrono::nanoseconds::zero())
        return absl::InvalidArgumentError("cut_probe_width must be positive");
    if(options.before && options.before->physical_ns < 0)
        return absl::InvalidArgumentError("invalid latest before");
    deadline = detail::deadline(config, deadline);
    LatestResult result;
    auto& page = result.page;
    const Hlc floor = sdk.causalFloor();
    size_t calls = 0;
    if(options.before)
        result.as_of = *options.before;
    else
    {
        auto cut = verifiedCut(sdk,
                               story,
                               floor,
                               config.cut_probe_width,
                               options.limits,
                               options.max_read_calls,
                               deadline,
                               record);
        if(!cut.ok())
            return cut.status();
        calls = cut->calls;
        if(!cut->as_of)
        {
            page = std::move(cut->probe);
            return result;
        }
        result.as_of = cut->as_of;
    }
    page.range = client::HlcRange{Hlc{}, *result.as_of};
    page.cut_covers_causal_floor = *result.as_of > floor;

    // answer holds the proven suffix oldest to newest; candidates from an incomplete Read are prepended provisionally.
    std::deque<Event> answer;
    auto keep = [&](std::vector<Event>& events)
    {
        for(auto it = events.rbegin(); it != events.rend() && answer.size() < n; ++it)
            if(!match || match(*it))
                answer.push_front(std::move(*it));
    };
    const PageLimits search{options.limits.max_events, std::numeric_limits<size_t>::max()};
    Hlc hi = *result.as_of;
    int64_t width = config.cut_probe_width.count();
    Hlc lo{hi.physical_ns - std::min(hi.physical_ns, width), 0};
    bool proven = false;
    for(;;)
    {
        if(hi <= Hlc{})
        {
            proven = true;
            break;
        }
        if(calls >= options.max_read_calls)
        {
            page.limited = DeliveryLimit::ReadCalls;
            break;
        }
        Page read = readPage(sdk, story, {lo, hi}, search, deadline, record);
        ++calls;
        page.completion = read.completion;
        page.completion_range = read.completion_range;
        page.stream_status = read.stream_status;
        if(read.answer_complete)
        {
            keep(read.events);
            if(answer.size() >= n || lo <= Hlc{})
            {
                proven = true;
                break;
            }
            width = std::max<int64_t>(1,
                                      hi.physical_ns - lo.physical_ns > std::numeric_limits<int64_t>::max() / 2
                                              ? std::numeric_limits<int64_t>::max()
                                              : 2 * (hi.physical_ns - lo.physical_ns));
            hi = lo;
            lo = {hi.physical_ns - std::min(hi.physical_ns, width), 0};
            continue;
        }
        if(read.delivered_prefix_end && *read.delivered_prefix_end > lo)
        {
            // A certified TRUNCATED prefix: narrow the suffix instead of paging forward through it.
            lo = std::max(*read.delivered_prefix_end, Hlc{lo.physical_ns + (hi.physical_ns - lo.physical_ns) / 2, 0});
            continue;
        }
        keep(read.events);
        break;
    }
    result.selection_complete = proven;
    page.events.assign(std::make_move_iterator(answer.begin()), std::make_move_iterator(answer.end()));
    page.raw_bytes = 0;
    for(const auto& event: page.events) page.raw_bytes += rawBytes(event.envelope);
    if(page.raw_bytes > options.limits.max_raw_bytes)
    {
        if(page.events.size() == 1)
            page.limited = DeliveryLimit::OversizedEvent;
        else
        {
            size_t drop = 0;
            while(page.events.size() - drop > 1 && page.raw_bytes > options.limits.max_raw_bytes)
                page.raw_bytes -= rawBytes(page.events[drop++].envelope);
            page.events.erase(page.events.begin(), page.events.begin() + static_cast<ptrdiff_t>(drop));
            page.limited = DeliveryLimit::Bytes;
            result.selection_complete = false;
        }
    }
    page.answer_complete = result.selection_complete;
    page.has_more = !result.selection_complete;
    return result;
}
absl::StatusOr<LatestResult> latestAggregate(client::Client& sdk,
                                             const ContextOptions& config,
                                             StoryId control,
                                             const std::function<bool(const Event&)>& is_aggregate,
                                             const LatestOptions& options,
                                             Deadline deadline)
{
    return latestSearch(sdk, config, control, 1, options, deadline, {}, is_aggregate);
}
} // namespace detail
absl::StatusOr<LatestResult> ContextSession::latest(size_t n, LatestOptions options, Deadline deadline)
{
    auto& core = *impl_->core;
    return detail::latestSearch(core.sdk, core.options, context().story_id, n, options, deadline, impl_->record);
}
} // namespace chronolog::context
