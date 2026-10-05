#include "player/replay/HotReplay.h"
#include "common/predicate/Predicate.h"
#include "player/adapter/EventConvert.h"
#include "chronolog/message_limits.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <tuple>
#include "absl/log/log.h"
#include "player/replay/CompletionPolicy.h"
#include "player/replay/ReplayMerge.h"
#include "player/replay/PhysicalRead.h"

namespace chronolog::player
{
namespace
{

bool inRange(const Range& r, const Event& e)
{
    if(r.axis == Range::Axis::Hlc)
        return e.hlc >= r.start && e.hlc < r.end;
    int64_t p = e.physical.physical_ns;
    if(!physicalBounded(e))
        return p >= r.start.physical_ns && p < r.end.physical_ns;
    uint64_t u = *e.physical.uncertainty_ns;
    if(u > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return true;
    int64_t bound = static_cast<int64_t>(u);
    int64_t lo = p < std::numeric_limits<int64_t>::min() + bound ? std::numeric_limits<int64_t>::min() : p - bound;
    int64_t hi = p > std::numeric_limits<int64_t>::max() - bound ? std::numeric_limits<int64_t>::max() : p + bound;
    return lo < r.end.physical_ns && hi >= r.start.physical_ns;
}

// A Keeper answers in ReplayLess order, and a stable sort of an ordered input moves every event for nothing.
void sortReplay(std::vector<Event>& events)
{
    if(!std::is_sorted(events.begin(), events.end(), ReplayLess))
        std::stable_sort(events.begin(), events.end(), ReplayLess);
}

bool abandoned(const HotFetch& fetch, const Range& range)
{
    for(const auto& lost: fetch.abandoned)
        if(range.axis == Range::Axis::Physical || (lost.start < range.end && lost.end > range.start))
            return true;
    return false;
}

bool routeAnswered(const HotFetch& f)
{
    return std::all_of(f.keepers.begin(),
                       f.keepers.end(),
                       [&](const KeeperFetch& k)
                       {
                           return k.frontier.answered &&
                                  k.frontier.epoch ==
                                          (k.frontier.expected_epoch ? k.frontier.expected_epoch : f.route_epoch);
                       });
}

// Keeps events accepted by `keep`, sorted per Keeper, merged and deduplicated.
template <class Pred>
std::vector<Event> mergeKept(HotFetch& f, Pred keep)
{
    std::vector<std::vector<Event>> inputs;
    inputs.reserve(f.keepers.size());
    for(auto& k: f.keepers)
    {
        std::vector<Event> kept;
        for(auto& e: k.events)
            if(keep(e))
                kept.push_back(std::move(e));
        std::stable_sort(kept.begin(), kept.end(), ReplayLess);
        inputs.push_back(std::move(kept));
    }
    return mergeReplay(std::move(inputs));
}

Hlc archiveEnd(const HotFetch& fetch, const Range& range)
{
    if(!routeAnswered(fetch) || fetch.keepers.empty())
        return range.end;
    Hlc boundary = fetch.archived_below;
    for(const auto& keeper: fetch.keepers) boundary = std::max(boundary, keeper.frontier.evicted_below);
    if(range.axis == Range::Axis::Physical)
        return boundary > Hlc{} ? range.end : range.start;
    return std::min(range.end, boundary);
}

// A Read admitted before a story was destroyed may reach archive files the Grapher has since erased, and nothing in the
// manifest says which windows were affected. The tombstone line precedes every Deleted record it causes, so a view that
// shows a removed file also shows the tombstone; checked after the archive reads (I6.7, I13.11).
bool archiveTombstoned(const HotReplayOptions& options, StoryId story)
{
    if(!options.archive)
        return false;
    const auto tombstoned = options.archive->tombstoned(story);
    return !tombstoned.ok() || *tombstoned;
}

// The end of the longest prefix of `range`, no later than `end`, that no source or abandoned range leaves open: a source
// that did not answer with its epoch cuts it to the start, a seal below `end` and a truncated answer cut it there, and
// so does the start of an abandoned range (I6.12, I6.13, I4.15). A truncated Read applies it once, a Tail every round.
Hlc prefixCut(const Range& range,
              Hlc end,
              Epoch route_epoch,
              const std::vector<KeeperFrontier>& frontiers,
              const std::vector<Range>& abandoned)
{
    Hlc cut = end;
    for(const auto& k: frontiers)
    {
        if(!k.answered || k.epoch != (k.expected_epoch ? k.expected_epoch : route_epoch))
            cut = range.start;
        else
        {
            if(!k.predecessor || k.sealed < k.own_cut)
                cut = std::min(cut, k.sealed);
            if(k.truncated)
                cut = std::min(cut, k.truncated_at.value_or(range.start));
        }
    }
    for(const auto& lost: abandoned)
        if(lost.start < cut && lost.end > range.start)
            cut = std::min(cut, lost.start);
    return cut;
}

size_t payloadBytes(const std::vector<Event>& events)
{
    size_t bytes = 0;
    for(const auto& e: events) bytes += e.envelope.payload.size();
    return bytes;
}

// Consume prefetched records in manifest order; callers retain their sequential range and cap decisions.
class ArchiveBatch
{
public:
    ArchiveBatch(const FileTierStore& archive, std::span<const ManifestRecord> records, const EventPredicate& predicate)
        : archive_(archive)
        , records_(records)
        , predicate_(predicate)
    {}

    // `cap` counts the events that match the predicate (I6.17), so a file is decoded whole when there is one.
    absl::StatusOr<std::vector<Event>> read(size_t index, Range range, size_t cap = SIZE_MAX)
    {
        const size_t decode_cap = predicate_.empty() ? cap : SIZE_MAX;
#ifdef CHRONOLOG_SEQUENTIAL_ARCHIVE_READS
        auto result = archive_.readRecord(records_[index], range, decode_cap);
#else
        if(index >= first_ + results_.size())
        {
            first_ = index;
            const size_t count = std::min(archive_.readConcurrency(), records_.size() - index);
            results_ = archive_.readRecords(records_.subspan(index, count), range, decode_cap);
        }
        auto result = std::move(results_[index - first_]);
        if(result.ok())
            // A sorted wider prefix contains the same prefix of every subsequently narrowed end bound.
            std::erase_if(*result, [&](const Event& event) { return !inRange(range, event); });
#endif
        if(result.ok())
        {
            std::erase_if(*result, [&](const Event& event) { return !predicate_.matches(event); });
            if(result->size() > cap)
                result->resize(cap);
        }
        return result;
    }

private:
    const FileTierStore& archive_;
    std::span<const ManifestRecord> records_;
    const EventPredicate& predicate_;
#ifndef CHRONOLOG_SEQUENTIAL_ARCHIVE_READS
    size_t first_{};
    std::vector<absl::StatusOr<std::vector<Event>>> results_;
#endif
};

bool loadArchive(const HotReplayOptions& options,
                 StoryId story,
                 Hlc from,
                 Hlc& bound,
                 const HotFetch& fetch,
                 size_t byte_limit,
                 bool& warned,
                 const EventPredicate& predicate,
                 std::vector<Event>& events)
{
    Hlc end = archiveEnd(fetch, Range{Range::Axis::Hlc, from, bound});
    if(end <= from)
        return true;
    if(!options.archive || !options.archive->refreshNow().ok())
        return false;
    auto manifest = options.archive->manifest(story);
    if(!manifest.ok())
        return false;
    std::stable_sort(manifest->begin(),
                     manifest->end(),
                     [](const auto& a, const auto& b)
                     { return std::tie(a.start, a.file) < std::tie(b.start, b.file); });
    std::erase_if(*manifest,
                  [&](const ManifestRecord& record)
                  { return record.state != ManifestState::Published || record.end <= from || record.start >= end; });
    ArchiveBatch batch(*options.archive, *manifest, predicate);
    for(size_t i = 0; i < manifest->size(); ++i)
    {
        const auto& record = (*manifest)[i];
        if(record.start >= end)
            continue;
        auto part = batch.read(i, Range{Range::Axis::Hlc, from, end}, options.read_max_events + 1);
        if(!part.ok())
            return false;
        if(part->size() == options.read_max_events + 1 && part->front().hlc == part->back().hlc &&
           (events.empty() || part->front().hlc <= events.front().hlc))
        {
            // The count probe ended inside the first tie group; read that group's whole exclusive window.
            Hlc group_end = part->front().hlc;
            if(group_end.logical == UINT32_MAX)
                group_end = {group_end.physical_ns + 1, 0};
            else
                ++group_end.logical;
            ArchiveBatch tie(*options.archive, std::span<const ManifestRecord>(&record, 1), predicate);
            part = tie.read(0, Range{Range::Axis::Hlc, part->front().hlc, group_end});
            if(!part.ok())
                return false;
            bound = std::min(bound, group_end);
            end = std::min(end, bound);
        }
        std::vector<std::vector<Event>> inputs;
        inputs.push_back(std::move(events));
        inputs.push_back(*std::move(part));
        events = mergeReplay(std::move(inputs));
        size_t bytes = 0, kept = 0;
        while(kept < events.size() && (kept == 0 || events[kept].hlc == events[0].hlc ||
                                       (kept < options.read_max_events && bytes <= byte_limit &&
                                        events[kept].envelope.payload.size() <= byte_limit - bytes)))
        {
            bytes += events[kept].envelope.payload.size();
            ++kept;
        }
        if(bytes > byte_limit && !warned)
        {
            warned = true;
            LOG(WARNING) << "Tail archive payload exceeds byte share for story " << story;
        }
        if(kept < events.size())
        {
            bound = std::min(bound, events[kept].hlc);
            end = std::min(end, bound);
            std::erase_if(events, [&](const Event& e) { return e.hlc >= bound; });
        }
    }
    if(end <= from)
        return !archiveTombstoned(options, story);
    auto lost = options.archive->incomplete(story, Range{Range::Axis::Hlc, from, end});
    return lost.ok() && !*lost && !archiveTombstoned(options, story);
}

class HotReplayStream final: public ReplayStream
{
    struct Head
    {
        size_t input;
        size_t pos;
    };
    struct After
    {
        const std::vector<std::vector<Event>>* inputs;
        bool operator()(const Head& a, const Head& b) const
        {
            return ReplayLess((*inputs)[b.input][b.pos], (*inputs)[a.input][a.pos]);
        }
    };

public:
    HotReplayStream(std::vector<std::vector<Event>> inputs, Completion completion, size_t batch_size)
        : inputs_(std::move(inputs))
        , heads_(After{&inputs_})
        , completion_(std::move(completion))
        , batch_size_(std::max<size_t>(batch_size, 1))
    {
        for(size_t i = 0; i < inputs_.size(); ++i)
            if(!inputs_[i].empty())
                heads_.push({i, 0});
    }

    absl::StatusOr<std::optional<ReplayBatch>> next() override
    {
        if(cancelled_.load())
            return absl::CancelledError("replay stream cancelled");
        ReplayBatch batch;
        size_t bytes = 0;
        while((pending_ || !heads_.empty()) && batch.events.size() < batch_size_)
        {
            Event e;
            if(pending_)
            {
                e = std::move(*pending_);
                pending_.reset();
            }
            else
            {
                e = pop();
                while(!heads_.empty() && inputs_[heads_.top().input][heads_.top().pos].id == e.id)
                    e.durability = std::max(e.durability, pop().durability);
                if(seen_.contains(e.id))
                    continue;
            }
            const size_t event_bytes = convert::encodedSize(e);
            if(!batch.events.empty() && bytes + event_bytes > kEventBatchBytes)
            {
                pending_ = std::move(e);
                break;
            }
            seen_.insert(e.id);
            bytes += event_bytes;
            batch.events.push_back(std::move(e));
        }
        if(!batch.events.empty())
            return std::optional<ReplayBatch>(std::move(batch));
        if(!completion_sent_)
        {
            completion_sent_ = true;
            batch.completion = completion_;
            return std::optional<ReplayBatch>(std::move(batch));
        }
        return std::optional<ReplayBatch>();
    }
    void cancel() override { cancelled_.store(true); }

private:
    Event pop()
    {
        Head h = heads_.top();
        heads_.pop();
        Event e = std::move(inputs_[h.input][h.pos]);
        if(++h.pos < inputs_[h.input].size())
            heads_.push(h);
        else
            std::vector<Event>().swap(inputs_[h.input]);
        return e;
    }
    std::vector<std::vector<Event>> inputs_;
    std::priority_queue<Head, std::vector<Head>, After> heads_;
    std::set<EventId> seen_;
    std::optional<Event> pending_;
    Completion completion_;
    size_t batch_size_;
    bool completion_sent_{};
    std::atomic<bool> cancelled_{false};
};

absl::StatusOr<std::unique_ptr<ReplayStream>> physicalRead(StoryId story,
                                                           const Range& range,
                                                           HotFetch& fetch,
                                                           const HotReplayOptions& options,
                                                           const HotSource& source,
                                                           const EventPredicate& predicate)
{
    const size_t limit = std::max<size_t>(1, options.read_max_events);
    size_t retained = 0;
    bool limited = false, unbounded = false, archive_failed = false;
    std::vector<KeeperFrontier> frontiers;
    std::vector<std::vector<Event>> inputs;
    auto take = [&](std::vector<Event> events)
    {
        std::erase_if(events, [&](const Event& e) { return !inRange(range, e) || !predicate.matches(e); });
        if(events.size() > limit - retained)
        {
            limited = true;
            events.resize(limit - retained);
        }
        retained += events.size();
        for(const auto& e: events) unbounded |= !physicalBounded(e);
        sortReplay(events);
        inputs.push_back(std::move(events));
    };
    bool policy = fetch.physical_policy;
    std::vector<ManifestRecord> records;
    if(options.archive || archiveEnd(fetch, range) > range.start)
    {
        archive_failed = !options.archive || !options.archive->refreshNow().ok();
        if(!archive_failed)
        {
            auto manifest = options.archive->manifest(story);
            archive_failed =
                    !manifest.ok() && !(absl::IsNotFound(manifest.status()) && archiveEnd(fetch, range) <= range.start);
            if(manifest.ok())
                records = *std::move(manifest);
            for(const auto& record: records) policy &= record.physical_policy;
        }
    }
    if(!policy && fetch.physical_policy)
    {
        auto full = source.fetchPhysicalMatching(story, range, false, predicate);
        if(!full.ok())
            return full.status();
        fetch = *std::move(full);
        fetch.physical_policy = false;
    }
    for(auto& keeper: fetch.keepers)
    {
        frontiers.push_back(keeper.frontier);
        take(std::move(keeper.events));
    }
    const auto scan = physicalWindow(range, policy);
    std::vector<ManifestRecord> selected;
    for(const auto& record: records)
    {
        if(record.end <= scan.start || record.start >= scan.end)
            continue;
        if(record.state == ManifestState::Lost)
            archive_failed = true;
        if(record.state == ManifestState::Published)
            selected.push_back(record);
    }
    if(!selected.empty())
    {
        ArchiveBatch batch(*options.archive, selected, predicate);
        for(size_t i = 0; i < selected.size(); ++i)
        {
            auto events = batch.read(i, range, limit - retained + 1);
            if(!events.ok())
                archive_failed = true;
            else
                take(*std::move(events));
        }
    }
    if(!records.empty() && archiveTombstoned(options, story))
        archive_failed = true;
    auto completion = CompletionPolicy::decide(range,
                                               fetch.route_epoch,
                                               frontiers,
                                               fetch.writers,
                                               archive_failed || abandoned(fetch, range),
                                               policy,
                                               unbounded || physicalEndSaturated(range));
    if(limited && completion.reason != IncompleteReason::SourceFailed)
    {
        completion.complete = false;
        completion.reason = IncompleteReason::Truncated;
    }
    return std::unique_ptr<ReplayStream>(
            std::make_unique<HotReplayStream>(std::move(inputs), std::move(completion), options.batch_size));
}

// A Tail delivers every event below its frontier T, once, in ReplayLess order (I6.13). T starts at the position's hlc and
// only grows. Each source answers a round from where its last answer ended and everything that answer covers is final
// (I6.11(a), (e)), so it is kept per source and instance until T passes it; a poll where any source did not answer, or a
// source restarted, moves nothing.
class TailStream final: public ProgressReplayStream
{
    struct Source
    {
        std::string instance;
        // [asked from, covered) is final for this source and held in `events`; the next ask starts at `covered`.
        Hlc covered;
        std::vector<Event> events;
        KeeperFrontier retained{};
        bool full{};
        size_t byte_limit{std::numeric_limits<size_t>::max()};
    };

public:
    TailStream(std::shared_ptr<const HotSource> source,
               StoryId story,
               Event position,
               HotReplayOptions options,
               EventPredicate predicate,
               bool progress)
        : source_(std::move(source))
        , story_(story)
        , options_(std::move(options))
        , position_(std::move(position))
        , predicate_(std::move(predicate))
        , frontier_(position_.hlc)
        , progress_enabled_(progress)
    {
        options_.batch_size = std::max<size_t>(options_.batch_size, 1);
        options_.read_max_events = std::max<size_t>(options_.read_max_events, 2);
    }

    // Polls once at open time so NOT_FOUND surfaces from tail().
    absl::Status open()
    {
        auto fetched = source_->fetchTailMatching(story_, frontier_, {}, predicate_);
        if(!fetched.ok())
            return fetched.status();
        absorb(*fetched);
        return absl::OkStatus();
    }

    absl::StatusOr<std::optional<ReplayBatch>> next() override
    {
        std::unique_lock lk(mu_);
        emitted_progress_.reset();
        for(;;)
        {
            if(cancelled_.load())
                return finish();
            if(pending_pos_ < pending_.size())
                return takeBatch();
            if(pending_progress_)
            {
                emitted_progress_ = pending_progress_;
                pending_progress_.reset();
                return std::optional<ReplayBatch>(ReplayBatch{});
            }
            if(suspect_)
            {
                // Only the Catalog says a story is destroyed; a Keeper's refusal or an archive tombstone is evidence.
                suspect_ = false;
                lk.unlock();
                const absl::Status live = options_.story_live ? options_.story_live(story_) : absl::OkStatus();
                lk.lock();
                if(cancelled_.load())
                    continue;
                confirmed_live_ = live.ok();
                if(absl::IsFailedPrecondition(live))
                    return live;
            }
            if(closed_)
                return finish();
            if(idle_)
            {
                cv_.wait_for(lk, options_.tail_poll, [&] { return cancelled_.load(); });
                idle_ = false;
                continue;
            }
            const Hlc from = frontier_;
            TailStarts starts;
            for(auto& [id, source]: sources_)
            {
                if(source.events.empty())
                    source.full = false;
                starts[id] = std::max(source.covered, from);
                if(source.full)
                    starts.retained[id] = source.retained;
            }
            lk.unlock();
            auto fetched = source_->fetchTailMatching(story_, from, starts, predicate_);
            const bool queued = fetched.ok() && !cancelled_.load() && absorb(*fetched);
            lk.lock();
            if(cancelled_.load())
                continue;
            if(!fetched.ok())
                return fetched.status();
            idle_ = !queued;
        }
    }

    void cancel() override
    {
        cancelled_.store(true);
        std::lock_guard lk(mu_);
        cv_.notify_all();
    }

    std::optional<Hlc> progress() const override { return emitted_progress_; }

private:
    bool afterPosition(const Event& event) const
    {
        if(position_.id.writer_id == 0 && position_.id.incarnation == 0 && position_.id.sequence == 0)
            return event.hlc >= position_.hlc;
        return ReplayLess(position_, event);
    }

    static bool answered(const KeeperFrontier& f, Epoch route_epoch)
    {
        return f.answered && f.epoch == (f.expected_epoch ? f.expected_epoch : route_epoch);
    }

    // True when T has reached an abandoned range: its events are gone and the Tail cannot go on gap-free (I4.15).
    bool reachedAbandoned(const std::vector<Range>& abandoned) const
    {
        return std::any_of(abandoned.begin(),
                           abandoned.end(),
                           [&](const Range& lost) { return lost.start <= frontier_ && lost.end > frontier_; });
    }

    void fail(IncompleteReason reason)
    {
        closed_ = true;
        final_reason_ = reason;
    }

    // Folds one source's reply into its buffer and returns the frontier it contributes to the round.
    KeeperFrontier takeReply(KeeperFetch& reply, Epoch route_epoch, size_t byte_limit)
    {
        KeeperFrontier f = reply.frontier;
        f.truncated = false;
        f.truncated_at.reset();
        if(!answered(f, route_epoch))
            return f;
        const SourceId id{f.process_id, f.predecessor ? f.expected_epoch : Epoch{}};
        auto [it, fresh_source] = sources_.try_emplace(id, Source{f.instance, frontier_, {}});
        Source& source = it->second;
        if(!fresh_source && source.instance != f.instance)
        {
            // A restarted instance never saw the bound its predecessor was asked from: start over from T.
            source = Source{f.instance, frontier_, {}};
            f.answered = false;
            return f;
        }
        const Hlc from = std::max(source.covered, frontier_);
        Hlc covered = f.predecessor ? std::min(f.sealed, f.own_cut) : f.sealed;
        std::vector<Event> fresh;
        Hlc last = from;
        for(auto& e: reply.events)
        {
            if(e.hlc < from)
                continue;
            last = std::max(last, e.hlc);
            if(afterPosition(e) && predicate_.matches(e))
                fresh.push_back(std::move(e));
        }
        // A truncated answer ends at the last event it returned, which may share its hlc with events it cut off.
        if(reply.frontier.truncated)
            covered = std::min(covered, last);
        covered = std::max(covered, source.covered);
        std::erase_if(fresh, [&](const Event& e) { return e.hlc >= covered; });
        std::stable_sort(fresh.begin(), fresh.end(), ReplayLess);
        // The buffer is bounded: what does not fit is asked for again once T has made room.
        const size_t room = options_.read_max_events - std::min(options_.read_max_events, source.events.size());
        size_t bytes = payloadBytes(source.events), keep = 0;
        while(keep < fresh.size() && keep < room &&
              ((source.events.empty() && keep == 0) ||
               (bytes <= byte_limit && fresh[keep].envelope.payload.size() <= byte_limit - bytes)))
        {
            bytes += fresh[keep].envelope.payload.size();
            ++keep;
        }
        if(keep < fresh.size())
        {
            source.full = true;
            covered = std::max(from, std::min(covered, fresh[keep].hlc));
            std::erase_if(fresh, [&](const Event& e) { return e.hlc >= covered; });
        }
        source.events.insert(source.events.end(),
                             std::make_move_iterator(fresh.begin()),
                             std::make_move_iterator(fresh.end()));
        if(payloadBytes(source.events) > byte_limit && warned_sources_.insert(id).second)
            LOG(WARNING) << "Tail Keeper payload exceeds byte share for story " << story_ << ", source "
                         << f.process_id;
        source.byte_limit = byte_limit;
        source.covered = covered;
        f.sealed = covered;
        source.full |= source.events.size() >= options_.read_max_events || payloadBytes(source.events) >= byte_limit;
        if(source.events.empty())
            source.full = false;
        source.retained = f;
        return f;
    }

    // Returns true when events were queued.
    bool absorb(HotFetch& fetch)
    {
        if(fetch.closed)
            closed_ = true;
        std::set<std::pair<SourceId, std::string>> refusals;
        for(const auto& k: fetch.keepers)
            if(k.frontier.status == absl::StatusCode::kFailedPrecondition)
            {
                const SourceId id{k.frontier.process_id, k.frontier.predecessor ? k.frontier.expected_epoch : Epoch{}};
                auto instance = k.frontier.instance;
                if(instance.empty())
                    if(auto it = sources_.find(id); it != sources_.end())
                        instance = it->second.instance;
                refusals.emplace(id, std::move(instance));
            }
        suspect_ = (!refusals.empty() && (!confirmed_live_ || refusals != refusals_)) || archiveRecordsTombstone();
        refusals_ = std::move(refusals);
        if(reachedAbandoned(fetch.abandoned))
        {
            fail(IncompleteReason::SourceFailed);
            return false;
        }
        std::vector<KeeperFrontier> frontiers;
        std::set<SourceId> asked, buffered;
        for(const auto& [id, source]: sources_) buffered.insert(id);
        for(const auto& k: fetch.keepers)
            buffered.insert(
                    SourceId{k.frontier.process_id, k.frontier.predecessor ? k.frontier.expected_epoch : Epoch{}});
        const size_t byte_limit =
                options_.tail_max_bytes / std::max<size_t>(1, buffered.size() + (options_.archive ? 1 : 0));
        std::set<SourceId> reset;
        for(auto& [id, source]: sources_)
            if(byte_limit < source.byte_limit && payloadBytes(source.events) > byte_limit)
            {
                source = Source{source.instance, frontier_, {}};
                reset.insert(id);
            }
        for(auto& k: fetch.keepers)
        {
            const SourceId id{k.frontier.process_id, k.frontier.predecessor ? k.frontier.expected_epoch : Epoch{}};
            asked.insert(id);
            if(reset.contains(id))
                k.frontier.answered = false;
            frontiers.push_back(takeReply(k, fetch.route_epoch, byte_limit));
        }
        // I6.13: every source has answered below `bound` and no abandoned range lies there; a Route with no Keeper
        // proves nothing.
        const Range range{Range::Axis::Hlc, frontier_, maxHlc()};
        Hlc bound = prefixCut(range, maxHlc(), fetch.route_epoch, frontiers, fetch.abandoned);
        bound = !reset.empty() || fetch.keepers.empty() || bound == maxHlc() ? frontier_ : std::max(bound, frontier_);
        std::vector<Event> cold;
        if(!loadArchive(options_, story_, frontier_, bound, fetch, byte_limit, archive_warned_, predicate_, cold))
        {
            suspect_ |= archiveRecordsTombstone();
            fail(IncompleteReason::SourceFailed);
            return false;
        }
        std::erase_if(cold, [&](const Event& e) { return e.hlc < frontier_ || e.hlc >= bound || !afterPosition(e); });
        std::vector<std::vector<Event>> inputs;
        inputs.push_back(std::move(cold));
        for(auto it = sources_.begin(); it != sources_.end();)
        {
            auto& held = it->second.events;
            auto split =
                    std::lower_bound(held.begin(), held.end(), bound, [](const Event& e, Hlc h) { return e.hlc < h; });
            inputs.emplace_back(std::make_move_iterator(held.begin()), std::make_move_iterator(split));
            if(split != held.begin())
                it->second.full = false;
            held.erase(held.begin(), split);
            if(held.empty())
                it->second.full = false;
            it = held.empty() && !asked.contains(it->first) ? sources_.erase(it) : std::next(it);
        }
        auto events = mergeReplay(std::move(inputs));
        const bool advanced = bound > frontier_;
        frontier_ = bound;
        if(reachedAbandoned(fetch.abandoned))
            fail(IncompleteReason::SourceFailed);
        if(events.empty())
        {
            if(progress_enabled_ && advanced)
                pending_progress_ = frontier_;
            return false;
        }
        pending_ = std::move(events);
        pending_pos_ = 0;
        return true;
    }

    bool archiveRecordsTombstone() const
    {
        if(!options_.archive)
            return false;
        const auto tombstoned = options_.archive->tombstoned(story_);
        return tombstoned.ok() && *tombstoned;
    }

    std::optional<ReplayBatch> takeBatch()
    {
        ReplayBatch batch;
        size_t bytes = 0;
        while(pending_pos_ < pending_.size() && batch.events.size() < std::max<size_t>(options_.batch_size, 1))
        {
            const size_t event_bytes = convert::encodedSize(pending_[pending_pos_]);
            if(!batch.events.empty() && bytes + event_bytes > kEventBatchBytes)
                break;
            bytes += event_bytes;
            batch.events.push_back(std::move(pending_[pending_pos_++]));
        }
        return batch;
    }

    std::optional<ReplayBatch> finish()
    {
        if(final_sent_)
            return std::nullopt;
        final_sent_ = true;
        pending_.clear();
        pending_pos_ = 0;
        ReplayBatch batch;
        batch.completion = Completion{false, frontier_, {}, final_reason_};
        return batch;
    }

    const std::shared_ptr<const HotSource> source_;
    const StoryId story_;
    HotReplayOptions options_;
    const Event position_;
    const EventPredicate predicate_;
    std::mutex mu_;
    std::condition_variable cv_;
    // Every event below this has been delivered or precedes the position.
    Hlc frontier_;
    const bool progress_enabled_;
    std::optional<Hlc> pending_progress_;
    std::optional<Hlc> emitted_progress_;
    std::map<SourceId, Source> sources_;
    std::vector<Event> pending_;
    size_t pending_pos_{};
    std::atomic<bool> cancelled_{false};
    bool closed_{};
    bool idle_{};
    bool suspect_{};
    bool confirmed_live_{};
    std::set<std::pair<SourceId, std::string>> refusals_;
    std::set<SourceId> warned_sources_;
    bool archive_warned_{};
    bool final_sent_{};
    IncompleteReason final_reason_{IncompleteReason::None};
};

} // namespace

HotReplay::HotReplay(std::shared_ptr<const HotSource> source, HotReplayOptions options)
    : source_(std::move(source))
    , options_(options)
{}

absl::StatusOr<std::unique_ptr<ReplayStream>> HotReplay::read(StoryId id, Range range) const
{
    return read(id, range, 0);
}

absl::StatusOr<std::unique_ptr<ReplayStream>> HotReplay::read(StoryId id, Range range, size_t max_events) const
{
    return read(id, range, max_events, {});
}

absl::StatusOr<std::unique_ptr<ReplayStream>>
HotReplay::read(StoryId id, Range range, const EventPredicate& predicate) const
{
    return read(id, range, 0, predicate);
}

absl::StatusOr<std::unique_ptr<ReplayStream>>
HotReplay::read(StoryId id, Range range, size_t max_events, const EventPredicate& predicate) const
{
    if(auto valid = predicate.validate(); !valid.ok())
        return valid;
    const size_t limit = std::max<size_t>(1, max_events ? max_events : options_.read_max_events);
    if(range.end < range.start)
        return absl::InvalidArgumentError("range end precedes start");
    bool archive_policy = true;
    if(range.axis == Range::Axis::Physical && options_.archive)
    {
        if(options_.archive->refreshNow().ok())
        {
            auto manifest = options_.archive->manifest(id);
            if(manifest.ok())
                for(const auto& record: *manifest) archive_policy &= record.physical_policy;
        }
    }
    auto fetched = range.axis == Range::Axis::Physical
                           ? source_->fetchPhysicalMatching(id, range, archive_policy, predicate)
                           : source_->fetchReadMatching(id, range, limit, predicate);
    if(!fetched.ok())
        return fetched.status();
    if(range.axis == Range::Axis::Physical)
    {
        fetched->physical_policy &= archive_policy;
        auto physical_options = options_;
        physical_options.read_max_events = limit;
        return physicalRead(id, range, *fetched, physical_options, *source_, predicate);
    }
    Range covered = range;
    bool limited = false;
    std::vector<KeeperFrontier> frontiers;
    std::vector<Hlc> hot_times;
    for(auto& k: fetched->keepers)
    {
        if(k.frontier.truncated && !k.frontier.truncated_at)
            k.frontier.truncated_at = k.events.empty() ? range.start : k.events.back().hlc;
        frontiers.push_back(k.frontier);
        std::erase_if(k.events, [&](const Event& e) { return !inRange(range, e) || !predicate.matches(e); });
        for(const auto& e: k.events)
            hot_times.push_back(range.axis == Range::Axis::Hlc ? e.hlc : Hlc{e.physical.physical_ns, 0});
    }
    if(hot_times.size() > limit)
    {
        std::sort(hot_times.begin(), hot_times.end());
        auto next = std::upper_bound(hot_times.begin(), hot_times.end(), hot_times[limit - 1]);
        if(next != hot_times.end())
        {
            covered.end = *next;
            limited = true;
        }
    }
    auto hotCount = [&](Hlc end)
    { return static_cast<size_t>(std::count_if(hot_times.begin(), hot_times.end(), [&](Hlc t) { return t < end; })); };
    bool source_truncated = std::any_of(frontiers.begin(), frontiers.end(), [](const auto& k) { return k.truncated; });
    Hlc prefix_cap = range.end;
    auto capPrefix = [&]
    {
        if(range.axis != Range::Axis::Hlc)
            return;
        const Hlc cut = prefixCut(range, covered.end, fetched->route_epoch, frontiers, fetched->abandoned);
        prefix_cap = std::min(prefix_cap, cut);
        covered.end = std::max(range.start, cut);
    };
    if(limited || source_truncated)
        capPrefix();
    const Hlc boundary = archiveEnd(*fetched, range);
    bool archive_ok = true;
    std::vector<ManifestRecord> records;
    std::vector<ManifestRecord> selected;
    if(boundary > range.start)
    {
        archive_ok = options_.archive && options_.archive->refreshNow().ok();
        if(archive_ok)
        {
            auto manifest = options_.archive->manifest(id);
            archive_ok = manifest.ok();
            if(manifest.ok())
                records = *std::move(manifest);
        }
        std::stable_sort(records.begin(),
                         records.end(),
                         [](const auto& a, const auto& b)
                         { return std::tie(a.start, a.file) < std::tie(b.start, b.file); });
        std::vector<ManifestRecord> published;
        for(const auto& record: records)
        {
            if(record.state != ManifestState::Published)
                continue;
            if(range.axis == Range::Axis::Hlc && (record.end <= range.start || record.start >= range.end))
                continue;
            published.push_back(record);
        }
        size_t archived_count = 0;
        Hlc accepted_cut = range.start;
        for(size_t i = 0; i < published.size();)
        {
            if(published[i].start >= covered.end || published[i].start >= boundary)
                break;
            size_t j = i;
            // A file counts its events, an upper bound on its matches, so a predicate can truncate a Read early.
            size_t group_count = 0;
            Hlc group_end = published[i].end;
            do {
                group_end = std::max(group_end, published[j].end);
                group_count += published[j++].event_count;
            } while(j < published.size() && published[j].start < group_end);
            group_end = std::min(range.end, group_end);
            if(!selected.empty() && covered.end < group_end)
            {
                covered.end = std::max(range.start, published[i].start);
                limited = true;
                break;
            }
            // I6.12: the first overlap component must fit whole, including a continuation inside it.
            if(selected.empty() && covered.end < group_end)
            {
                covered.end = group_end;
                limited = group_end < range.end;
                prefix_cap = range.end;
                if(prefixCut(range, group_end, fetched->route_epoch, frontiers, fetched->abandoned) < group_end &&
                   source_truncated)
                {
                    // The archive's mandatory prefix may exceed the target at the hot sources too.
                    auto prefix = source_->fetchReadMatching(id,
                                                             Range{Range::Axis::Hlc, range.start, group_end},
                                                             SIZE_MAX,
                                                             predicate);
                    if(!prefix.ok())
                        return prefix.status();
                    if(prefix->route_epoch != fetched->route_epoch ||
                       prefix->keepers.size() != fetched->keepers.size() ||
                       prefix->archived_below > fetched->archived_below)
                        return absl::UnavailableError("route changed while admitting archive prefix");
                    for(size_t k = 0; k < prefix->keepers.size(); ++k)
                        if(prefix->keepers[k].frontier.instance != fetched->keepers[k].frontier.instance ||
                           prefix->keepers[k].frontier.process_id != fetched->keepers[k].frontier.process_id ||
                           prefix->keepers[k].frontier.expected_epoch != fetched->keepers[k].frontier.expected_epoch ||
                           prefix->keepers[k].frontier.predecessor != fetched->keepers[k].frontier.predecessor ||
                           prefix->keepers[k].frontier.own_cut != fetched->keepers[k].frontier.own_cut ||
                           prefix->keepers[k].frontier.evicted_below > fetched->keepers[k].frontier.evicted_below)
                            return absl::UnavailableError("source changed while admitting archive prefix");
                    fetched = std::move(prefix);
                    frontiers.clear();
                    hot_times.clear();
                    source_truncated = false;
                    for(auto& k: fetched->keepers)
                    {
                        if(k.frontier.truncated && !k.frontier.truncated_at)
                            k.frontier.truncated_at = k.events.empty() ? range.start : k.events.back().hlc;
                        frontiers.push_back(k.frontier);
                        source_truncated |= k.frontier.truncated;
                        for(const auto& e: k.events)
                            if(inRange(range, e) && predicate.matches(e))
                                hot_times.push_back(e.hlc);
                    }
                    prefix_cap = range.end;
                    limited = group_end < range.end;
                }
            }
            Hlc candidate_cut = j < published.size() ? std::min(covered.end, published[j].start) : covered.end;
            candidate_cut = std::max(range.start, candidate_cut);
            size_t hot_count = hotCount(candidate_cut);
            bool fits = archived_count <= limit && group_count <= limit - archived_count &&
                        hot_count <= limit - archived_count - group_count;
            bool first_component = selected.empty();
            if(!fits && !first_component)
            {
                covered.end = accepted_cut;
                limited = true;
                break;
            }
            selected.insert(selected.end(), published.begin() + i, published.begin() + j);
            archived_count += group_count;
            accepted_cut = candidate_cut;
            i = j;
        }
    }
    if(limited || source_truncated)
    {
        capPrefix();
        if(range.axis == Range::Axis::Hlc)
        {
            for(const auto& record: records)
                if(record.state == ManifestState::Lost && record.start < covered.end && record.end > range.start)
                {
                    prefix_cap = std::min(prefix_cap, record.start);
                    covered.end = std::max(range.start, record.start);
                }
            bool changed;
            do {
                changed = false;
                for(const auto& record: records)
                    if(record.state == ManifestState::Published && record.start < covered.end &&
                       record.end > covered.end && covered.end < range.end)
                    {
                        prefix_cap = std::min(prefix_cap, record.start);
                        Hlc cut = std::max(range.start, record.start);
                        changed |= cut < covered.end;
                        covered.end = cut;
                    }
            } while(changed);
        }
    }
    std::vector<std::vector<Event>> inputs;
    bool unbounded_event = false;
    for(auto& k: fetched->keepers)
    {
        std::erase_if(k.events, [&](const Event& e) { return !inRange(covered, e) || !predicate.matches(e); });
        for(const auto& e: k.events) unbounded_event |= !physicalBounded(e);
        sortReplay(k.events);
        inputs.push_back(std::move(k.events));
    }
    Range cold{range.axis, range.start, std::min(boundary, covered.end)};
    if(cold.start < cold.end && archive_ok && options_.archive)
    {
        for(const auto& record: records)
            if(record.state == ManifestState::Lost && record.start < cold.end && record.end > cold.start)
                archive_ok = false;
        std::erase_if(selected, [&](const ManifestRecord& record) { return record.start >= cold.end; });
        ArchiveBatch batch(*options_.archive, selected, predicate);
        for(size_t i = 0; i < selected.size(); ++i)
        {
            auto events = batch.read(i, cold);
            if(!events.ok())
                archive_ok = false;
            else
            {
                std::erase_if(*events, [&](const Event& e) { return !inRange(cold, e); });
                for(const auto& e: *events) unbounded_event |= !physicalBounded(e);
                inputs.push_back(*std::move(events));
            }
        }
        if(!records.empty() && archiveTombstoned(options_, id))
            archive_ok = false;
    }
    Completion completion = CompletionPolicy::decide(covered,
                                                     fetched->route_epoch,
                                                     frontiers,
                                                     fetched->writers,
                                                     !archive_ok || abandoned(*fetched, range),
                                                     fetched->physical_policy,
                                                     unbounded_event);
    if((limited || source_truncated) && completion.reason != IncompleteReason::SourceFailed)
    {
        completion.complete = false;
        completion.reason = IncompleteReason::Truncated;
        completion.frontier = covered.end;
    }
    if((limited || source_truncated) && range.axis == Range::Axis::Hlc)
        completion.frontier = std::min(covered.end, prefix_cap);
    return std::unique_ptr<ReplayStream>(
            std::make_unique<HotReplayStream>(std::move(inputs), std::move(completion), options_.batch_size));
}

absl::StatusOr<std::unique_ptr<ReplayStream>> HotReplay::tail(StoryId id, Event position) const
{
    return tail(id, std::move(position), EventPredicate{}, false);
}

absl::StatusOr<std::unique_ptr<ReplayStream>> HotReplay::tail(StoryId id, Event position, bool progress) const
{
    return tail(id, std::move(position), EventPredicate{}, progress);
}

absl::StatusOr<std::unique_ptr<ReplayStream>>
HotReplay::tail(StoryId id, Event position, const EventPredicate& predicate) const
{
    return tail(id, std::move(position), predicate, false);
}

absl::StatusOr<std::unique_ptr<ReplayStream>>
HotReplay::tail(StoryId id, Event position, const EventPredicate& predicate, bool progress) const
{
    if(position.id == EventId{})
        position.id.story_id = id;
    if(position.id.story_id != id)
        return absl::InvalidArgumentError("position belongs to a different story");
    if(auto valid = predicate.validate(); !valid.ok())
        return valid;
    auto stream = std::make_unique<TailStream>(source_, id, std::move(position), options_, predicate, progress);
    if(auto opened = stream->open(); !opened.ok())
        return opened;
    return std::unique_ptr<ReplayStream>(std::move(stream));
}

} // namespace chronolog::player
