#include "internal.h"
#include <limits>
#include <nlohmann/json.hpp>

namespace chronolog::context
{
namespace detail
{
Page readPage(client::Client& sdk,
              StoryId story,
              client::HlcRange range,
              PageLimits limits,
              Deadline deadline,
              std::shared_ptr<Record> record)
{
    Page page;
    page.range = range;
    page.completion_range = range;
    auto stream = sdk.read(story,
                           range,
                           client::ReadOptions{static_cast<uint32_t>(
                                   std::min<size_t>(limits.max_events, std::numeric_limits<uint32_t>::max()))},
                           deadline);
    if(!stream.ok())
    {
        page.stream_status = stream.status();
        page.has_more = true;
        return page;
    }
    IoRegistration registration(std::move(record), [&] { stream->cancel(); });
    std::optional<Hlc> omitted;
    std::optional<Event> previous;
    for(;;)
    {
        auto item = stream->next(deadline);
        if(!item.ok())
        {
            page.stream_status = item.status();
            break;
        }
        if(!*item)
            break;
        if((**item).completion)
            page.completion = (**item).completion;
        for(auto& event: (**item).events)
        {
            if(event.id.story_id != story || event.hlc < range.start || event.hlc >= range.end ||
               (previous && !ReplayLess(*previous, event)))
            {
                page.stream_status = absl::DataLossError("invalid or unordered recall event");
                stream->cancel();
                return page;
            }
            previous = event;
            const size_t bytes = rawBytes(event.envelope);
            if(!omitted && !page.events.empty() && event.hlc != page.events.back().hlc &&
               (page.events.size() >= limits.max_events ||
                bytes > limits.max_raw_bytes - std::min(page.raw_bytes, limits.max_raw_bytes)))
            {
                omitted = event.hlc;
                if(page.limited != DeliveryLimit::OversizedEvent)
                    page.limited =
                            page.events.size() >= limits.max_events ? DeliveryLimit::Events : DeliveryLimit::Bytes;
            }
            if(!omitted)
            {
                page.raw_bytes += bytes;
                if(page.raw_bytes > limits.max_raw_bytes)
                    page.limited = DeliveryLimit::OversizedEvent;
                else if(page.events.size() >= limits.max_events)
                    page.limited = DeliveryLimit::Events;
                page.events.push_back(std::move(event));
            }
        }
    }
    const bool certified = page.stream_status.ok() && page.completion &&
                           (page.completion->complete || page.completion->reason == IncompleteReason::Truncated);
    if(certified)
    {
        Hlc end = page.completion->complete ? range.end : std::min(range.end, page.completion->frontier);
        if(omitted)
            end = std::min(end, *omitted);
        std::erase_if(page.events, [&](const Event& event) { return event.hlc >= end; });
        page.raw_bytes = 0;
        for(const auto& event: page.events) page.raw_bytes += rawBytes(event.envelope);
        if(end > range.start)
        {
            page.delivered_prefix_end = end;
            if(!page.events.empty())
                page.after = Position{page.events.back().hlc, page.events.back().id};
        }
        page.has_more = end < range.end;
        page.answer_complete = page.completion->complete && end == range.end;
    }
    else
        page.has_more = true;
    return page;
}
} // namespace detail
namespace
{
using Json = nlohmann::json;
constexpr std::string_view cursor_prefix = "chronolog-recall/v1:";
struct Cursor
{
    StoryId story;
    client::HlcRange range;
    Hlc next;
};
std::string encodeCursor(const Cursor& cursor)
{
    return std::string(cursor_prefix) + Json::array({cursor.story,
                                                     cursor.range.start.physical_ns,
                                                     cursor.range.start.logical,
                                                     cursor.range.end.physical_ns,
                                                     cursor.range.end.logical,
                                                     cursor.next.physical_ns,
                                                     cursor.next.logical})
                                                .dump();
}
absl::StatusOr<Cursor> decodeCursor(const std::string& token)
{
    if(!token.starts_with(cursor_prefix) || token.size() > 512)
        return absl::InvalidArgumentError("invalid recall cursor kind");
    const auto values = Json::parse(token.substr(cursor_prefix.size()), nullptr, false);
    if(!values.is_array() || values.size() != 7)
        return absl::InvalidArgumentError("invalid recall cursor");
    for(size_t i = 0; i < values.size(); ++i)
    {
        if(!values[i].is_number_integer() ||
           (values[i].is_number_integer() && !values[i].is_number_unsigned() && values[i].get<int64_t>() < 0))
            return absl::InvalidArgumentError("invalid recall cursor number");
        const uint64_t bound = i == 0 ? std::numeric_limits<uint64_t>::max()
                                      : (i % 2 == 0 ? std::numeric_limits<uint32_t>::max()
                                                    : static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
        if(values[i].get<uint64_t>() > bound)
            return absl::InvalidArgumentError("recall cursor overflow");
    }
    Cursor cursor{values[0].get<StoryId>(),
                  {{values[1].get<int64_t>(), values[2].get<uint32_t>()},
                   {values[3].get<int64_t>(), values[4].get<uint32_t>()}},
                  {values[5].get<int64_t>(), values[6].get<uint32_t>()}};
    if(!cursor.story || cursor.range.start >= cursor.range.end || cursor.next < cursor.range.start ||
       cursor.next >= cursor.range.end)
        return absl::InvalidArgumentError("invalid recall cursor range");
    return cursor;
}
} // namespace
absl::StatusOr<Page> ContextSession::recall(RecallOptions options, Deadline deadline)
{
    auto& core = *impl_->core;
    const auto story = context().story_id;
    deadline = detail::deadline(core.options, deadline);
    if(!options.limits.max_events || !options.limits.max_raw_bytes || !options.max_read_calls)
        return absl::InvalidArgumentError("recall limits must be positive");
    const Hlc floor = core.sdk.causalFloor();
    client::HlcRange original{options.start.value_or(Hlc{}), options.end.value_or(Hlc{})};
    Hlc start = original.start;
    size_t calls = 0;
    if(options.cursor)
    {
        auto cursor = decodeCursor(*options.cursor);
        if(!cursor.ok())
            return cursor.status();
        if(cursor->story != story || (options.start && *options.start != cursor->range.start) ||
           (options.end && *options.end != cursor->range.end))
            return absl::InvalidArgumentError("cursor generation or range mismatch");
        original = cursor->range;
        start = cursor->next;
    }
    else if(!options.end)
    {
        auto next_floor = detail::successor(floor);
        if(!next_floor.ok())
            return next_floor.status();
        Hlc candidate = std::max(*next_floor, detail::realtime());
        Page probe;
        bool verified = false;
        while(calls < options.max_read_calls)
        {
            const auto range = detail::probeRange(candidate, core.options.cut_probe_width);
            if(range.start >= range.end)
                break;
            probe = detail::readPage(core.sdk, story, range, options.limits, deadline, impl_->record);
            ++calls;
            if(!probe.stream_status.ok() || !probe.completion)
                break;
            if(probe.completion->complete)
            {
                original.end = std::max(candidate, probe.completion->frontier);
                verified = true;
                break;
            }
            const auto frontier = probe.completion->frontier;
            if(frontier <= Hlc{} || frontier == candidate)
                break;
            candidate = frontier;
        }
        if(!verified)
        {
            probe.events.clear();
            probe.raw_bytes = 0;
            probe.after.reset();
            probe.delivered_prefix_end.reset();
            probe.next_cursor.reset();
            probe.answer_complete = false;
            probe.has_more = true;
            probe.range.reset();
            probe.cut_covers_causal_floor = false;
            if(calls >= options.max_read_calls)
                probe.limited = DeliveryLimit::ReadCalls;
            return probe;
        }
    }
    if(original.start.physical_ns < 0 || original.end.physical_ns < 0 || original.end < original.start)
        return absl::InvalidArgumentError("invalid recall range");
    if(calls >= options.max_read_calls)
    {
        Page page;
        page.range = original;
        page.limited = DeliveryLimit::ReadCalls;
        page.has_more = true;
        page.cut_covers_causal_floor = original.end > floor;
        return page;
    }
    Page page = detail::readPage(core.sdk, story, {start, original.end}, options.limits, deadline, impl_->record);
    page.range = original;
    page.cut_covers_causal_floor = original.end > floor;
    if(page.has_more && page.delivered_prefix_end && *page.delivered_prefix_end > start)
        page.next_cursor = encodeCursor({story, original, *page.delivered_prefix_end});
    return page;
}
} // namespace chronolog::context
