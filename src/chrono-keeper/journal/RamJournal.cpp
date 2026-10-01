#include "journal/RamJournal.h"

#include <algorithm>

namespace chronolog
{
namespace
{

std::string ExpectedSequence(uint64_t next) { return "expected sequence " + std::to_string(next); }

} // namespace

RamJournal::RamJournal(std::shared_ptr<Clock> clock,
                       std::shared_ptr<const Membership> membership,
                       RamJournalConfig config)
    : clock_(std::move(clock))
    , membership_(std::move(membership))
    , config_(config)
{}

absl::Status RamJournal::requireStory(StoryId id) const
{
    auto route = membership_->route(id);
    return route.status();
}

std::optional<Route> RamJournal::currentRoute(StoryId id) const
{
    auto route = membership_->route(id);
    if(!route.ok())
        return std::nullopt;
    return *route;
}

absl::Status RamJournal::registerWriter(StoryId story, uint64_t writer_id, uint64_t incarnation)
{
    if(story == 0 || writer_id == 0 || incarnation == 0)
        return absl::InvalidArgumentError("incomplete writer identity");
    auto& sh = shard(story);
    std::unique_lock lock(sh.mu);
    auto& st = sh.stories[story];
    auto it = st.slots.find(writer_id);
    if(it != st.slots.end())
    {
        if(incarnation < it->second.incarnation)
            return absl::FailedPreconditionError("older incarnation");
        if(incarnation == it->second.incarnation)
        {
            it->second.assigned = true;
            return absl::OkStatus();
        }
    }
    auto writer = std::make_shared<Writer>();
    writer->writer_id = writer_id;
    writer->incarnation = incarnation;
    st.writers[{writer_id, incarnation}] = writer;
    st.slots[writer_id] = Slot{incarnation, true, writer};
    return absl::OkStatus();
}

void RamJournal::unassignWriter(StoryId story, uint64_t writer_id)
{
    auto& sh = shard(story);
    std::unique_lock lock(sh.mu);
    auto it = sh.stories.find(story);
    if(it == sh.stories.end())
        return;
    auto slot = it->second.slots.find(writer_id);
    if(slot != it->second.slots.end())
        slot->second.assigned = false;
}

void RamJournal::releaseWriter(StoryId story, uint64_t writer_id, uint64_t incarnation)
{
    std::shared_ptr<Writer> writer;
    {
        auto& sh = shard(story);
        std::shared_lock lock(sh.mu);
        auto it = sh.stories.find(story);
        if(it == sh.stories.end())
            return;
        auto w = it->second.writers.find({writer_id, incarnation});
        if(w != it->second.writers.end())
            writer = w->second;
    }
    if(!writer)
        return;
    std::lock_guard lock(writer->mu);
    writer->released = true;
}

AppendResult RamJournal::appendOne(StoryId story,
                                   const AppendItem& item,
                                   Durability durability,
                                   int64_t now_ns,
                                   std::set<std::pair<uint64_t, uint64_t>>& poisoned)
{
    AppendResult result;
    result.id = EventId{story, item.writer_id, item.incarnation, item.sequence};
    auto fail = [&](absl::Status status, bool with_route = false)
    {
        result.status = std::move(status);
        if(with_route)
            result.current_route = currentRoute(story);
        return result;
    };

    if(item.writer_id == 0 || item.incarnation == 0 || item.sequence == 0)
        return fail(absl::InvalidArgumentError("event id needs writer_id, incarnation and sequence"));
    if(durability != Durability::Accepted)
        return fail(absl::UnimplementedError("requested durability is not built; request ACCEPTED"));

    std::shared_ptr<Writer> writer;
    {
        auto& sh = shard(story);
        std::shared_lock lock(sh.mu);
        auto it = sh.stories.find(story);
        if(it == sh.stories.end())
            return fail(absl::FailedPreconditionError("writer not registered at this keeper"), true);
        auto slot = it->second.slots.find(item.writer_id);
        if(slot == it->second.slots.end())
            return fail(absl::FailedPreconditionError("writer not registered at this keeper"), true);
        if(!slot->second.assigned)
            return fail(absl::FailedPreconditionError("writer is not assigned to this keeper"), true);
        if(item.incarnation < slot->second.incarnation)
            return fail(absl::FailedPreconditionError("incarnation is older than the one acquired"));
        if(item.incarnation > slot->second.incarnation)
            return fail(absl::FailedPreconditionError("incarnation not yet registered at this keeper"), true);
        writer = slot->second.current;
    }

    std::lock_guard lock(writer->mu);
    if(writer->released)
        return fail(absl::FailedPreconditionError("incarnation is released"));
    const std::pair<uint64_t, uint64_t> key{item.writer_id, item.incarnation};
    if(poisoned.count(key))
        return fail(absl::FailedPreconditionError("an earlier item of this writer in the batch was rejected; " +
                                                  ExpectedSequence(writer->next_sequence)));
    if(item.sequence < writer->next_sequence)
    {
        uint64_t oldest = writer->next_sequence - writer->window.size();
        if(item.sequence < oldest)
            return fail(absl::FailedPreconditionError("sequence is outside the dedupe window; " +
                                                      ExpectedSequence(writer->next_sequence)));
        result.status = absl::OkStatus();
        result.achieved = Durability::Accepted;
        result.hlc = writer->window[item.sequence - oldest];
        return result;
    }
    if(item.sequence > writer->next_sequence)
    {
        poisoned.insert(key);
        return fail(absl::FailedPreconditionError(ExpectedSequence(writer->next_sequence)));
    }

    if(item.envelope.payload.size() > config_.payload_max_bytes)
        return fail(absl::InvalidArgumentError("payload exceeds the configured maximum"));
    if(!item.envelope.trace_id.empty() && item.envelope.trace_id.size() != 16)
        return fail(absl::InvalidArgumentError("trace_id must be 16 bytes"));
    if(!item.envelope.span_id.empty() && item.envelope.span_id.size() != 8)
        return fail(absl::InvalidArgumentError("span_id must be 8 bytes"));
    if(item.causal_floor.physical_ns < 0 || item.causal_floor.physical_ns - now_ns > config_.causal_floor_skew_limit_ns)
        return fail(absl::InvalidArgumentError("causal_floor is beyond the skew limit"));

    Hlc hlc = clock_->observe(std::max(item.causal_floor, writer->last_hlc));
    Event event;
    event.id = result.id;
    event.physical = item.physical;
    event.hlc = hlc;
    event.envelope = item.envelope;
    event.durability = Durability::Accepted;
    writer->events.push_back(std::move(event));
    writer->last_hlc = hlc;
    ++writer->next_sequence;
    writer->window.push_back(hlc);
    while(writer->window.size() > std::max<size_t>(config_.dedupe_window, 1)) writer->window.pop_front();

    result.status = absl::OkStatus();
    result.achieved = Durability::Accepted;
    result.hlc = hlc;
    return result;
}

absl::StatusOr<std::vector<AppendResult>> RamJournal::append(const AppendBatch& batch, Durability durability)
{
    if(batch.story_id == 0)
        return absl::InvalidArgumentError("story_id is required");
    if(batch.items.empty())
        return absl::InvalidArgumentError("batch has no items");
    if(durability != Durability::Unspecified && durability != Durability::Accepted && durability != Durability::Durable)
        return absl::InvalidArgumentError("unknown durability");

    std::vector<AppendResult> results;
    results.reserve(batch.items.size());
    absl::Status epoch = membership_->validateEpoch(batch.story_id, batch.epoch);
    if(!epoch.ok())
    {
        if(epoch.code() != absl::StatusCode::kFailedPrecondition && epoch.code() != absl::StatusCode::kNotFound)
            return epoch;
        auto route =
                epoch.code() == absl::StatusCode::kFailedPrecondition ? currentRoute(batch.story_id) : std::nullopt;
        for(const auto& item: batch.items)
        {
            AppendResult r;
            r.id = EventId{batch.story_id, item.writer_id, item.incarnation, item.sequence};
            r.status = epoch;
            r.current_route = route;
            results.push_back(std::move(r));
        }
        return results;
    }

    auto reading = clock_->now();
    if(!reading.ok())
        return absl::UnavailableError("clock unavailable");

    std::set<std::pair<uint64_t, uint64_t>> poisoned;
    for(const auto& item: batch.items)
        results.push_back(appendOne(batch.story_id, item, durability, reading->physical_ns, poisoned));
    return results;
}

absl::StatusOr<std::vector<Event>> RamJournal::read(StoryId id, Range range) const
{
    if(range.start > range.end)
        return absl::InvalidArgumentError("range start is after end");
    if(auto s = requireStory(id); !s.ok())
        return s;

    std::vector<std::shared_ptr<Writer>> writers;
    {
        auto& sh = shard(id);
        std::shared_lock lock(sh.mu);
        auto it = sh.stories.find(id);
        if(it != sh.stories.end())
            for(const auto& [key, writer]: it->second.writers) writers.push_back(writer);
    }

    std::vector<Event> out;
    for(const auto& writer: writers)
    {
        std::lock_guard lock(writer->mu);
        if(range.axis == Range::Axis::Hlc)
        {
            auto lo = std::lower_bound(writer->events.begin(),
                                       writer->events.end(),
                                       range.start,
                                       [](const Event& e, const Hlc& h) { return e.hlc < h; });
            for(auto it = lo; it != writer->events.end() && it->hlc < range.end; ++it) out.push_back(*it);
        }
        else
        {
            for(const auto& e: writer->events)
                if(e.physical.physical_ns >= range.start.physical_ns && e.physical.physical_ns < range.end.physical_ns)
                    out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end(), ReplayLess);
    return out;
}

Hlc RamJournal::seal(StoryId id, std::vector<std::shared_ptr<Writer>>& live) const
{
    // F is ticked before any writer lock is taken, so every assignment already made is below F
    // and, once its writer lock is acquired here, inserted. Every later assignment is above F.
    Hlc f = clock_->tick();
    std::vector<std::shared_ptr<Writer>> all;
    std::set<const Writer*> candidates;
    {
        auto& sh = shard(id);
        std::shared_lock lock(sh.mu);
        auto it = sh.stories.find(id);
        if(it != sh.stories.end())
        {
            for(const auto& [key, writer]: it->second.writers) all.push_back(writer);
            for(const auto& [writer_id, slot]: it->second.slots)
                if(slot.assigned)
                    candidates.insert(slot.current.get());
        }
    }
    // Every writer is synchronized, including superseded and unassigned ones whose assignment may be in flight.
    for(const auto& w: all)
    {
        std::lock_guard lock(w->mu);
        if(!w->released && candidates.count(w.get()))
            live.push_back(w);
    }
    return f;
}

absl::StatusOr<std::vector<Frontier>> RamJournal::frontier(StoryId id) const
{
    if(auto s = requireStory(id); !s.ok())
        return s;
    std::vector<std::shared_ptr<Writer>> live;
    Hlc f = seal(id, live);
    std::vector<Frontier> out;
    out.reserve(live.size());
    for(const auto& w: live) out.push_back(Frontier{w->writer_id, w->incarnation, f});
    return out;
}

absl::StatusOr<Hlc> RamJournal::keeperFrontier(StoryId id) const
{
    if(auto s = requireStory(id); !s.ok())
        return s;
    std::vector<std::shared_ptr<Writer>> live;
    return seal(id, live);
}

} // namespace chronolog
