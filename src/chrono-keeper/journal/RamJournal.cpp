#include "journal/RamJournal.h"
#include "clock/PhysicalPolicy.h"

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
{
    instance_ = config.instance;
}

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
    if(item.causal_floor.physical_ns < 0 ||
       (item.causal_floor.physical_ns > now_ns &&
        (now_ns < 0 || item.causal_floor.physical_ns - now_ns > config_.causal_floor_skew_limit_ns)))
        return fail(absl::InvalidArgumentError("causal_floor is beyond the skew limit"));

    auto interval = physicalInterval(item.physical);
    if(!interval.ok())
        return fail(interval.status());
    auto assignment = clock_->assignChecked(std::max(item.causal_floor, writer->last_hlc), *interval);
    if(!assignment.ok())
    {
        result = fail(assignment.status());
        if(absl::IsOutOfRange(result.status))
        {
            ++writer->next_sequence;
            writer->window[item.sequence] = result;
            while(writer->window.size() > std::max<size_t>(config_.dedupe_window, 1))
                writer->window.erase(writer->window.begin());
        }
        return result;
    }
    Hlc hlc = assignment->hlc;
    assignmentObserved(hlc);
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

std::vector<RamJournal::WriterCheckpoint> RamJournal::checkpointWriters() const
{
    std::vector<WriterCheckpoint> out;
    for(auto& sh: shards_)
    {
        std::shared_lock lock(sh.mu);
        for(const auto& [story_id, story]: sh.stories)
            for(const auto& [key, writer]: story.writers)
            {
                std::lock_guard writer_lock(writer->mu);
                const auto& slot = story.slots.at(key.first);
                WriterCheckpoint checkpoint{{story_id, key.first, key.second},
                                            writer->next_sequence,
                                            writer->last_hlc,
                                            writer->released,
                                            slot.current == writer && slot.assigned,
                                            {}};
                for(const auto& [sequence, result]: writer->window)
                    if((absl::IsOutOfRange(result.status) ||
                        (result.status.ok() && result.achieved == Durability::Durable)) &&
                       !writer->pending.contains(sequence))
                        checkpoint.window.push_back(result);
                out.push_back(std::move(checkpoint));
            }
    }
    return out;
}

void RamJournal::restoreWriter(const WriterCheckpoint& checkpoint)
{
    const auto& key = checkpoint.key;
    auto& sh = shard(key.story_id);
    std::unique_lock lock(sh.mu);
    auto& story = sh.stories[key.story_id];
    auto& writer = story.writers[{key.writer_id, key.incarnation}];
    if(!writer)
    {
        writer = std::make_shared<Writer>();
        writer->writer_id = key.writer_id;
        writer->incarnation = key.incarnation;
    }
    writer->next_sequence = std::max(writer->next_sequence, checkpoint.next_sequence);
    writer->last_hlc = std::max(writer->last_hlc, checkpoint.last_hlc);
    writer->released = writer->released || checkpoint.released;
    for(const auto& result: checkpoint.window) writer->window[result.id.sequence] = result;
    while(!writer->window.empty() && writer->next_sequence > std::max<size_t>(config_.dedupe_window, 1) &&
          writer->window.begin()->first < writer->next_sequence - std::max<size_t>(config_.dedupe_window, 1))
        writer->window.erase(writer->window.begin());
    auto& slot = story.slots[key.writer_id];
    if(slot.incarnation <= key.incarnation)
        slot = Slot{key.incarnation, checkpoint.assigned, writer};
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
    if(supportsDurable())
        done = [this, callback = std::move(done)](auto results) mutable
        { finishAppend(std::move(callback), std::move(results)); };
    if(batch.story_id == 0)
        return done(absl::InvalidArgumentError("story_id is required"));
    if(batch.items.empty())
        return done(absl::InvalidArgumentError("batch has no items"));
    if(!admission_ready_.load())
        return done(absl::UnavailableError("acquisition snapshot is not applied"));
    if(durability != Durability::Unspecified && durability != Durability::Accepted && durability != Durability::Durable)
        return done(absl::InvalidArgumentError("unknown durability"));

    std::vector<AppendResult> results;
    results.reserve(batch.items.size());
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
        const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.append_ceiling_wait_ms);
        std::optional<AppendResult> result;
        for(;;)
        {
            auto gate = admission(batch.story_id);
            std::shared_lock gate_lock(gate->gate);
            auto status = membership_->validateEpoch(batch.story_id, batch.epoch);
            auto route = currentRoute(batch.story_id);
            if(status.ok() && !config_.process_id.empty() && route &&
               std::none_of(route->keepers.begin(),
                            route->keepers.end(),
                            [&](const auto& k) { return k.process_id == config_.process_id; }))
                status = absl::FailedPreconditionError("keeper is not listed in the route");
            uint64_t generation;
            {
                std::lock_guard lock(dynamic_mu_);
                generation = ceiling_generation_;
                if(status.ok() && dynamic_ &&
                   (!scheduleSteps(*gate) || ceiling_ <= std::max(gate->state.ordering_cut, gate->observe_floor)))
                    status = absl::UnavailableError("route clock steps or ceiling deferred");
            }
            if(!status.ok())
            {
                result = AppendResult{};
                result->id = {batch.story_id,
                              batch.items[i].writer_id,
                              batch.items[i].incarnation,
                              batch.items[i].sequence};
                result->status = status;
                result->current_route = route;
                break;
            }
            auto reading = clock_->now();
            if(!reading.ok())
            {
                result = AppendResult{};
                result->id = {batch.story_id,
                              batch.items[i].writer_id,
                              batch.items[i].incarnation,
                              batch.items[i].sequence};
                result->status = absl::UnavailableError("clock unavailable");
                break;
            }
            result = appendOne(batch.story_id,
                               batch.items[i],
                               durability,
                               reading->physical_ns,
                               poisoned,
                               [state, i](AppendResult r) { state->finish(i, std::move(r)); });
            if(!result || result->status != Clock::wouldExceedCeiling())
                break;
            gate_lock.unlock();
            std::unique_lock lock(dynamic_mu_);
            ++ceiling_waiters_;
            const bool changed =
                    ceiling_cv_.wait_until(lock, deadline, [&] { return generation != ceiling_generation_; });
            --ceiling_waiters_;
            if(!changed)
            {
                result->status = absl::UnavailableError("ceiling wait timed out");
                break;
            }
        }
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

void RamJournal::scan(const Writer& writer, Range range, std::vector<Event>& out, std::optional<Range> physical_filter)
{
    auto emit = [&](const Event& event)
    {
        if(physical_filter)
        {
            auto interval = physicalInterval(event.physical);
            if(!interval.ok())
                return;
            const auto start = physical_filter->start.physical_ns, end = physical_filter->end.physical_ns;
            if(interval->bounded ? !(interval->lo < end && interval->hi >= start)
                                 : !(start <= event.physical.physical_ns && event.physical.physical_ns < end))
                return;
        }
        out.push_back(event);
    };
    if(range.axis == Range::Axis::Hlc)
    {
        auto first = std::lower_bound(writer.events.begin(),
                                      writer.events.end(),
                                      range.start,
                                      [](const Event& event, Hlc hlc) { return event.hlc < hlc; });
        for(auto it = first; it != writer.events.end() && it->hlc < range.end; ++it) emit(*it);
    }
    else
        for(const auto& event: writer.events)
            if(physical_filter || (event.physical.physical_ns >= range.start.physical_ns &&
                                   event.physical.physical_ns < range.end.physical_ns))
                emit(event);
}

Hlc RamJournal::seal(StoryId id,
                     std::vector<std::shared_ptr<Writer>>& live,
                     const Range* range,
                     std::vector<Event>* events,
                     std::optional<Hlc> tick,
                     std::optional<Range> physical_filter) const
{
    // F is ticked before any writer lock is taken, so every assignment already made is below F
    // and, once its writer lock is acquired here, inserted. Every later assignment is above F.
    Hlc f = tick ? *tick : reserveFrontier(clock_->tick());
    if(auto owner = retiredOwner(id))
        f = owner->own_cut;
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
                scan(*w, *range, *events, physical_filter);
            if(!w->pending.empty())
                f = std::min(f, w->pending.begin()->second.event.hlc);
            if(!w->released && candidates.count(w.get()))
                live.push_back(w);
        }
        writerScanned(WriterKey{id, w->writer_id, w->incarnation});
    }
    return capSeal(id, f);
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

absl::StatusOr<RamJournal::SealedRead>
RamJournal::sealedRead(StoryId id, Range range, std::optional<Hlc> tick, std::optional<Range> physical_filter) const
{
    if(range.start > range.end)
        return absl::InvalidArgumentError("range start is after end");
    if(auto status = requireStory(id); !status.ok())
        return status;
    std::vector<std::shared_ptr<Writer>> live;
    SealedRead out;
    out.view.sealed = seal(id, live, &range, &out.events, tick, physical_filter);
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
    {
        std::lock_guard lock(dynamic_mu_);
        for(const auto& [id, a]: admissions_)
        {
            (void)a;
            out.push_back(id);
        }
    }
    for(auto& shard: shards_)
    {
        std::shared_lock lock(shard.mu);
        for(const auto& [id, story]: shard.stories) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
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

namespace chronolog
{
absl::StatusOr<int64_t> RamJournal::physicalFrontier(StoryId id) const
{
    if(auto status = requireStory(id); !status.ok())
        return status;
    {
        std::lock_guard lock(dynamic_mu_);
        if(dynamic_ && !ceiling_granted_)
            return absl::UnavailableError("ceiling not granted");
    }
    std::lock_guard report_lock(physical_mu_);
    const auto acceptance = clock_->acceptanceClock();
    const auto window = PhysicalPolicy{}.acceptance_window_ns;
    int64_t frontier = acceptance < INT64_MIN + window ? INT64_MIN : acceptance - window;
    std::vector<std::shared_ptr<Writer>> writers;
    {
        auto& sh = shard(id);
        std::shared_lock lock(sh.mu);
        auto it = sh.stories.find(id);
        if(it != sh.stories.end())
            for(const auto& [key, writer]: it->second.writers) writers.push_back(writer);
    }
    for(const auto& writer: writers)
    {
        std::lock_guard lock(writer->mu);
        for(const auto& [sequence, pending]: writer->pending)
        {
            auto visible = writer->window.find(sequence);
            if(visible != writer->window.end() && visible->second.achieved == Durability::Accepted)
                continue;
            auto interval = physicalInterval(pending.event.physical);
            if(interval.ok())
                frontier = std::min(frontier, interval->lo);
        }
    }
    {
        std::lock_guard lock(dynamic_mu_);
        if(dynamic_)
        {
            int64_t cp = physical_ceiling_;
            auto it = admissions_.find(id);
            if(it != admissions_.end() &&
               std::none_of(it->second->state.route.keepers.begin(),
                            it->second->state.route.keepers.end(),
                            [&](const auto& k) { return k.process_id == config_.process_id; }))
            {
                int64_t own = INT64_MIN;
                for(const auto& owner: it->second->state.predecessors)
                    if(owner.instance == instance_ && owner.keeper.process_id == config_.process_id)
                        own = std::max(own, owner.own_physical_ceiling_ns);
                if(own != INT64_MIN)
                    cp = std::min(cp, own);
            }
            frontier = std::min(frontier, cp < INT64_MIN + window ? INT64_MIN : cp - window);
        }
    }
    auto [it, inserted] = physical_reports_.try_emplace(id, INT64_MIN);
    (void)inserted;
    auto persisted = reservePhysicalFrontier(id, std::max(frontier, it->second));
    if(!persisted.ok())
        return persisted.status();
    it->second = *persisted;
    return *persisted;
}
} // namespace chronolog

namespace chronolog
{
std::shared_ptr<RamJournal::Admission> RamJournal::admission(StoryId story) const
{
    std::lock_guard lock(dynamic_mu_);
    auto& state = admissions_[story];
    if(!state)
        state = std::make_shared<Admission>();
    return state;
}
void RamJournal::enableDynamic(std::string instance, Hlc floor, int64_t physical, int64_t bl, int64_t bh)
{
    std::lock_guard lock(dynamic_mu_);
    auto* control = dynamic_cast<CeilingControl*>(clock_.get());
    if(!control)
        throw std::invalid_argument("dynamic membership needs a ceiling clock");
    instance_ = std::move(instance);
    dynamic_ = true;
    restart_floor_ = std::max(restart_floor_, floor);
    restart_physical_ = std::max(restart_physical_, physical);
    acceptance_budget_ = bl;
    hlc_budget_ = bh;
    control->setCeiling(ceiling_);
}
void RamJournal::extendCeiling(Hlc ceiling, int64_t physical)
{
    std::lock_guard lock(dynamic_mu_);
    ceiling_granted_ = true;
    ceiling_ = std::max(ceiling_, ceiling);
    physical_ceiling_ = std::max(physical_ceiling_, physical);
    dynamic_cast<CeilingControl*>(clock_.get())->setCeiling(ceiling_);
    ++ceiling_generation_;
    ceiling_cv_.notify_all();
}
bool RamJournal::scheduleSteps(Admission& a)
{
    auto reading = clock_->now();
    if(!reading.ok() || !a.installed)
        return false;
    auto within = [&](int64_t floor, int64_t budget)
    {
        return floor <= reading->physical_ns ||
               (reading->physical_ns <= INT64_MAX - budget && floor <= reading->physical_ns + budget);
    };
    Hlc floor = std::max(restart_floor_, a.observe ? a.observe_floor : Hlc{});
    const int64_t physical = std::max(restart_physical_, a.raise ? a.physical_floor : int64_t{0});
    if(!within(floor.physical_ns, hlc_budget_) || !within(physical, acceptance_budget_) ||
       !within(clock_->acceptanceClock(), acceptance_budget_))
        return false;
    clock_->observeFloor(floor);
    clock_->raiseAcceptanceClock(physical);
    a.observe = a.raise = false;
    return true;
}
void RamJournal::applyRoute(StoryId story,
                            RouteState state,
                            bool observe,
                            uint64_t revision,
                            std::function<void()> install)
{
    auto a = admission(story);
    std::unique_lock gate_lock(a->gate);
    std::lock_guard lock(dynamic_mu_);
    if(a->installed && (revision < a->revision || state.route.epoch < a->state.route.epoch))
        return;
    auto listed = [&](const Route& route)
    {
        return std::any_of(route.keepers.begin(),
                           route.keepers.end(),
                           [&](const auto& k) { return k.process_id == config_.process_id; });
    };
    bool added = listed(state.route) && (!a->installed || !listed(a->state.route));
    observe = observe || (dynamic_ && added);
    if(observe)
        a->observe_floor = std::max(a->observe_floor, state.ordering_cut);
    if(added)
        a->physical_floor = std::max(a->physical_floor, state.physical_floor);
    a->observe = a->observe || observe;
    a->raise = a->raise || added;
    a->state = std::move(state);
    a->revision = revision;
    a->installed = true;
    install();
    if(dynamic_)
        (void)scheduleSteps(*a);
    ++ceiling_generation_;
    ceiling_cv_.notify_all();
}
void RamJournal::acknowledgeRoutes(uint64_t revision)
{
    std::lock_guard lock(dynamic_mu_);
    applied_route_revision_ = std::max(applied_route_revision_, revision);
}
uint64_t RamJournal::appliedRouteRevision() const
{
    std::lock_guard lock(dynamic_mu_);
    return applied_route_revision_;
}
std::string RamJournal::instance() const
{
    std::lock_guard lock(dynamic_mu_);
    return instance_;
}
bool RamJournal::dynamic() const
{
    std::lock_guard lock(dynamic_mu_);
    return dynamic_;
}
int64_t RamJournal::realtime() const
{
    auto r = clock_->now();
    return r.ok() ? r->physical_ns : 0;
}
Hlc RamJournal::wantedCeiling() const { return clock_->tick(); }
std::vector<Predecessor> RamJournal::predecessorOwners(StoryId story) const
{
    std::lock_guard lock(dynamic_mu_);
    std::vector<Predecessor> owners;
    auto it = admissions_.find(story);
    if(it != admissions_.end())
        for(const auto& p: it->second->state.predecessors)
            if(p.instance == instance_ && p.keeper.process_id == config_.process_id)
                owners.push_back(p);
    return owners;
}
std::optional<Predecessor> RamJournal::retiredOwner(StoryId story) const
{
    auto route = currentRoute(story);
    if(route && std::any_of(route->keepers.begin(),
                            route->keepers.end(),
                            [&](const auto& k) { return k.process_id == config_.process_id; }))
        return std::nullopt;
    auto owners = predecessorOwners(story);
    if(owners.empty())
        return std::nullopt;
    return *std::max_element(owners.begin(),
                             owners.end(),
                             [](const auto& a, const auto& b) { return a.own_cut < b.own_cut; });
}
Hlc RamJournal::capSeal(StoryId story, Hlc seal) const
{
    std::lock_guard lock(dynamic_mu_);
    if(!dynamic_)
        return seal;
    Hlc cap = ceiling_;
    auto it = admissions_.find(story);
    if(it != admissions_.end() && std::none_of(it->second->state.route.keepers.begin(),
                                               it->second->state.route.keepers.end(),
                                               [&](const auto& k) { return k.process_id == config_.process_id; }))
    {
        Hlc own{};
        for(const auto& p: it->second->state.predecessors)
            if(p.instance == instance_ && p.keeper.process_id == config_.process_id)
                own = std::max(own, p.own_cut);
        if(own != Hlc{})
            cap = std::min(cap, own);
    }
    return std::min(seal, cap);
}
bool RamJournal::retiredDrained(StoryId story) const
{
    auto p = retiredOwner(story);
    return p && evictionFloor(story) >= p->own_cut;
}
} // namespace chronolog

namespace chronolog
{
bool RamJournal::neverHeldEvent(StoryId story) const
{
    for(const auto& checkpoint: checkpointWriters())
        if(checkpoint.key.story_id == story && checkpoint.last_hlc != Hlc{})
            return false;
    return true;
}
} // namespace chronolog
