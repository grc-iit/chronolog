#include "journal/RamJournal.h"

#include <algorithm>
#include <future>

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

std::optional<AppendResult> RamJournal::appendOne(StoryId story,
                                                  const AppendItem& item,
                                                  Durability durability,
                                                  int64_t now_ns,
                                                  std::set<std::pair<uint64_t, uint64_t>>& poisoned,
                                                  std::function<void(AppendResult)> done)
{
    AppendResult result;
    result.id = EventId{story, item.writer_id, item.incarnation, item.sequence};
    auto fail = [&](absl::Status status, bool with_route = false)
    {
        result.status = std::move(status);
        result.achieved = Durability::Unspecified;
        if(with_route)
            result.current_route = currentRoute(story);
        return result;
    };

    if(item.writer_id == 0 || item.incarnation == 0 || item.sequence == 0)
        return fail(absl::InvalidArgumentError("event id needs writer_id, incarnation and sequence"));
    if(durability != Durability::Accepted && !supportsDurable())
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
        auto pending = writer->pending.find(item.sequence);
        if(pending != writer->pending.end())
        {
            pending->second.waiters.push_back(std::move(done));
            return std::nullopt;
        }
        auto original = writer->window.find(item.sequence);
        if(original == writer->window.end())
            return fail(absl::FailedPreconditionError("sequence is outside the dedupe window; " +
                                                      ExpectedSequence(writer->next_sequence)));
        result = original->second;
        if(durability != Durability::Accepted && result.achieved == Durability::Accepted)
        {
            if(!durableAvailable())
                return fail(absl::UnavailableError("WAL failed; DURABLE requires restart"));
            auto stored = std::lower_bound(writer->events.begin(),
                                           writer->events.end(),
                                           result.hlc,
                                           [](const Event& event, Hlc hlc) { return event.hlc < hlc; });
            if(stored == writer->events.end() || stored->id != result.id)
                return fail(absl::UnavailableError("original event has already left Keeper RAM"));
            Event event = *stored;
            event.durability = Durability::Durable;
            writer->pending.emplace(item.sequence, Pending{event, {std::move(done)}});
            persist(event,
                    [this, writer, sequence = item.sequence](absl::Status status)
                    { complete(writer, sequence, std::move(status)); });
            return std::nullopt;
        }
        return result;
    }
    if(item.sequence > writer->next_sequence)
    {
        poisoned.insert(key);
        return fail(absl::FailedPreconditionError(ExpectedSequence(writer->next_sequence)));
    }

    if(durability != Durability::Accepted && !durableAvailable())
        return fail(absl::UnavailableError("WAL failed; DURABLE requires restart"));

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
    event.durability = durability == Durability::Accepted ? Durability::Accepted : Durability::Durable;
    writer->last_hlc = hlc;
    ++writer->next_sequence;
    result.status = absl::OkStatus();
    result.achieved = event.durability;
    result.hlc = hlc;
    writer->window[item.sequence] = result;
    while(!writer->window.empty() && writer->next_sequence > std::max<size_t>(config_.dedupe_window, 1) &&
          writer->window.begin()->first < writer->next_sequence - std::max<size_t>(config_.dedupe_window, 1))
        writer->window.erase(writer->window.begin());
    if(event.durability == Durability::Accepted)
    {
        writer->events.push_back(std::move(event));
        return result;
    }
    writer->pending.emplace(item.sequence, Pending{event, {std::move(done)}});
    persist(event,
            [this, writer, sequence = item.sequence](absl::Status status)
            { complete(writer, sequence, std::move(status)); });
    return std::nullopt;
}

void RamJournal::persist(const Event&, std::function<void(absl::Status)>) {}

void RamJournal::complete(const std::shared_ptr<Writer>& writer, uint64_t sequence, absl::Status status)
{
    std::vector<std::function<void(AppendResult)>> waiters;
    AppendResult result;
    {
        std::lock_guard lock(writer->mu);
        auto pending = writer->pending.find(sequence);
        const auto& event = pending->second.event;
        result.id = event.id;
        result.hlc = event.hlc;
        result.status = std::move(status);
        if(result.status.ok())
        {
            result.achieved = Durability::Durable;
            auto pos = std::lower_bound(writer->events.begin(),
                                        writer->events.end(),
                                        event.hlc,
                                        [](const Event& e, Hlc h) { return e.hlc < h; });
            if(pos != writer->events.end() && pos->id == event.id)
                *pos = event;
            else
                writer->events.insert(pos, event);
        }
        auto original = writer->window.find(sequence);
        if(original != writer->window.end() &&
           (result.status.ok() || original->second.achieved != Durability::Accepted))
            original->second = result;
        waiters = std::move(pending->second.waiters);
        writer->pending.erase(pending);
    }
    for(auto& waiter: waiters) waiter(result);
}

void RamJournal::restore(const Event& event)
{
    auto& sh = shard(event.id.story_id);
    std::unique_lock lock(sh.mu);
    auto& st = sh.stories[event.id.story_id];
    auto& writer = st.writers[{event.id.writer_id, event.id.incarnation}];
    if(!writer)
    {
        writer = std::make_shared<Writer>();
        writer->writer_id = event.id.writer_id;
        writer->incarnation = event.id.incarnation;
    }
    auto& slot = st.slots[event.id.writer_id];
    if(slot.incarnation <= event.id.incarnation)
        slot = Slot{event.id.incarnation, true, writer};
    auto pos = std::lower_bound(writer->events.begin(),
                                writer->events.end(),
                                event.hlc,
                                [](const Event& stored, Hlc hlc) { return stored.hlc < hlc; });
    if(pos != writer->events.end() && pos->id == event.id)
        *pos = event;
    else
        writer->events.insert(pos, event);
    writer->last_hlc = std::max(writer->last_hlc, event.hlc);
    writer->next_sequence = std::max(writer->next_sequence, event.id.sequence + 1);
    writer->window[event.id.sequence] =
            AppendResult{absl::OkStatus(), event.durability, event.hlc, event.id, std::nullopt};
    while(!writer->window.empty() && writer->next_sequence > std::max<size_t>(config_.dedupe_window, 1) &&
          writer->window.begin()->first < writer->next_sequence - std::max<size_t>(config_.dedupe_window, 1))
        writer->window.erase(writer->window.begin());
}

absl::StatusOr<std::vector<AppendResult>> RamJournal::append(const AppendBatch& batch, Durability durability)
{
    std::promise<absl::StatusOr<std::vector<AppendResult>>> promise;
    auto future = promise.get_future();
    appendAsync(batch, durability, [&promise](auto result) { promise.set_value(std::move(result)); });
    return future.get();
}

void RamJournal::appendAsync(const AppendBatch& batch, Durability durability, AppendCallback done)
{
    if(durability != Durability::Accepted && supportsDurable())
        done = [this, callback = std::move(done)](auto results) mutable
        { finishAppend(std::move(callback), std::move(results)); };
    if(batch.story_id == 0)
        return done(absl::InvalidArgumentError("story_id is required"));
    if(batch.items.empty())
        return done(absl::InvalidArgumentError("batch has no items"));
    if(durability != Durability::Unspecified && durability != Durability::Accepted && durability != Durability::Durable)
        return done(absl::InvalidArgumentError("unknown durability"));

    std::vector<AppendResult> results;
    results.reserve(batch.items.size());
    absl::Status epoch = membership_->validateEpoch(batch.story_id, batch.epoch);
    if(!epoch.ok())
    {
        if(epoch.code() != absl::StatusCode::kFailedPrecondition && epoch.code() != absl::StatusCode::kNotFound)
            return done(epoch);
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
        return done(std::move(results));
    }

    auto reading = clock_->now();
    if(!reading.ok())
        return done(absl::UnavailableError("clock unavailable"));

    struct BatchState
    {
        std::mutex mu;
        std::vector<AppendResult> results;
        size_t remaining;
        AppendCallback done;
        void finish(size_t index, AppendResult result)
        {
            std::unique_lock lock(mu);
            if(index < results.size())
                results[index] = std::move(result);
            if(--remaining != 0)
                return;
            auto callback = std::move(done);
            auto out = std::move(results);
            lock.unlock();
            callback(std::move(out));
        }
    };
    auto state = std::make_shared<BatchState>();
    state->results.resize(batch.items.size());
    state->remaining = batch.items.size() + 1;
    state->done = std::move(done);
    std::set<std::pair<uint64_t, uint64_t>> poisoned;
    for(size_t i = 0; i < batch.items.size(); ++i)
    {
        auto result = appendOne(batch.story_id,
                                batch.items[i],
                                durability,
                                reading->physical_ns,
                                poisoned,
                                [state, i](AppendResult r) { state->finish(i, std::move(r)); });
        if(result)
            state->finish(i, std::move(*result));
    }
    state->finish(batch.items.size(), {});
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
        scan(*writer, range, out);
    }
    std::sort(out.begin(), out.end(), ReplayLess);
    return out;
}

void RamJournal::scan(const Writer& writer, Range range, std::vector<Event>& out)
{
    if(range.axis == Range::Axis::Hlc)
    {
        auto first = std::lower_bound(writer.events.begin(),
                                      writer.events.end(),
                                      range.start,
                                      [](const Event& event, Hlc hlc) { return event.hlc < hlc; });
        for(auto it = first; it != writer.events.end() && it->hlc < range.end; ++it) out.push_back(*it);
    }
    else
        for(const auto& event: writer.events)
            if(event.physical.physical_ns >= range.start.physical_ns &&
               event.physical.physical_ns < range.end.physical_ns)
                out.push_back(event);
}

Hlc RamJournal::seal(StoryId id,
                     std::vector<std::shared_ptr<Writer>>& live,
                     const Range* range,
                     std::vector<Event>* events,
                     std::optional<Hlc> tick) const
{
    // F is ticked before any writer lock is taken, so every assignment already made is below F
    // and, once its writer lock is acquired here, inserted. Every later assignment is above F.
    Hlc f = tick ? *tick : reserveFrontier(clock_->tick());
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
        {
            std::lock_guard lock(w->mu);
            if(range)
                scan(*w, *range, *events);
            if(!w->pending.empty())
                f = std::min(f, w->pending.begin()->second.event.hlc);
            if(!w->released && candidates.count(w.get()))
                live.push_back(w);
        }
        writerScanned(WriterKey{id, w->writer_id, w->incarnation});
    }
    return f;
}

absl::StatusOr<RamJournal::SealedView> RamJournal::sealedView(StoryId id) const
{
    if(auto s = requireStory(id); !s.ok())
        return s;
    std::vector<std::shared_ptr<Writer>> live;
    SealedView view;
    view.sealed = seal(id, live);
    view.frontiers.reserve(live.size());
    for(const auto& w: live) view.frontiers.push_back(Frontier{w->writer_id, w->incarnation, view.sealed});
    return view;
}

absl::StatusOr<RamJournal::SealedRead> RamJournal::sealedRead(StoryId id, Range range, std::optional<Hlc> tick) const
{
    if(range.start > range.end)
        return absl::InvalidArgumentError("range start is after end");
    if(auto status = requireStory(id); !status.ok())
        return status;
    std::vector<std::shared_ptr<Writer>> live;
    SealedRead out;
    out.view.sealed = seal(id, live, &range, &out.events, tick);
    for(const auto& writer: live)
        out.view.frontiers.push_back(Frontier{writer->writer_id, writer->incarnation, out.view.sealed});
    out.evicted_below = evictionFloor(id);
    std::sort(out.events.begin(), out.events.end(), ReplayLess);
    return out;
}

absl::StatusOr<std::vector<Frontier>> RamJournal::frontier(StoryId id) const
{
    auto view = sealedView(id);
    if(!view.ok())
        return view.status();
    return std::move(view->frontiers);
}

absl::StatusOr<Hlc> RamJournal::keeperFrontier(StoryId id) const
{
    auto view = sealedView(id);
    if(!view.ok())
        return view.status();
    return view->sealed;
}

std::vector<RamJournal::WriterKey> RamJournal::liveWriters() const
{
    std::vector<std::pair<StoryId, std::shared_ptr<Writer>>> current;
    for(auto& sh: shards_)
    {
        std::shared_lock lock(sh.mu);
        for(const auto& [story, st]: sh.stories)
            for(const auto& [writer_id, slot]: st.slots) current.emplace_back(story, slot.current);
    }
    std::vector<WriterKey> out;
    for(const auto& [story, w]: current)
    {
        std::lock_guard lock(w->mu);
        if(!w->released)
            out.push_back(WriterKey{story, w->writer_id, w->incarnation});
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace chronolog

namespace chronolog
{
std::vector<StoryId> RamJournal::storyIds() const
{
    std::vector<StoryId> out;
    for(auto& shard: shards_)
    {
        std::shared_lock lock(shard.mu);
        for(const auto& [id, story]: shard.stories) out.push_back(id);
    }
    return out;
}

void RamJournal::eraseEvents(StoryId story, Range range, bool advance_floor)
{
    std::vector<std::shared_ptr<Writer>> writers;
    {
        auto& sh = shard(story);
        std::unique_lock lock(sh.mu);
        if(advance_floor)
        {
            auto& state = sh.stories[story];
            state.evicted_below = std::max(state.evicted_below, range.end);
        }
        auto it = sh.stories.find(story);
        if(it != sh.stories.end())
            for(const auto& [key, writer]: it->second.writers) writers.push_back(writer);
    }
    for(const auto& writer: writers)
    {
        std::lock_guard lock(writer->mu);
        std::erase_if(writer->events,
                      [&](const Event& event) {
                          return event.hlc >= range.start && event.hlc < range.end &&
                                 !writer->pending.contains(event.id.sequence);
                      });
    }
}
} // namespace chronolog

namespace chronolog
{
Hlc RamJournal::evictionFloor(StoryId story) const
{
    auto& sh = shard(story);
    std::shared_lock lock(sh.mu);
    auto it = sh.stories.find(story);
    return it == sh.stories.end() ? Hlc{} : it->second.evicted_below;
}
} // namespace chronolog
