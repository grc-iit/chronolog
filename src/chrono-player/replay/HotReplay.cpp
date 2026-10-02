#include "chrono-player/replay/HotReplay.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <tuple>
#include "chrono-player/replay/CompletionPolicy.h"
#include "chrono-player/replay/ReplayMerge.h"
#include "chrono-player/replay/PhysicalRead.h"

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

bool loadArchive(const HotReplayOptions& options,
                 StoryId story,
                 const Range& range,
                 const HotFetch& fetch,
                 std::vector<Event>& events)
{
    Hlc end = archiveEnd(fetch, range);
    if(end <= range.start)
        return true;
    if(!options.archive)
        return false;
    if(!options.archive->refreshNow().ok())
        return false;
    Range cold{range.axis, range.start, end};
    auto archived = options.archive->read(story, cold);
    auto lost = options.archive->incomplete(story, cold);
    if(archived.ok())
        events = *std::move(archived);
    return archived.ok() && lost.ok() && !*lost && !archiveTombstoned(options, story);
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
        while(!heads_.empty() && batch.events.size() < batch_size_)
        {
            Event e = pop();
            while(!heads_.empty() && inputs_[heads_.top().input][heads_.top().pos].id == e.id)
                e.durability = std::max(e.durability, pop().durability);
            if(seen_.insert(e.id).second)
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
    Completion completion_;
    size_t batch_size_;
    bool completion_sent_{};
    std::atomic<bool> cancelled_{false};
};

absl::StatusOr<std::unique_ptr<ReplayStream>> physicalRead(StoryId story,
                                                           const Range& range,
                                                           HotFetch& fetch,
                                                           const HotReplayOptions& options,
                                                           const HotSource& source)
{
    const size_t limit = std::max<size_t>(1, options.read_max_events);
    size_t retained = 0;
    bool limited = false, unbounded = false, archive_failed = false;
    std::vector<KeeperFrontier> frontiers;
    std::vector<std::vector<Event>> inputs;
    auto take = [&](std::vector<Event> events)
    {
        std::erase_if(events, [&](const Event& e) { return !inRange(range, e); });
        if(events.size() > limit - retained)
        {
            limited = true;
            events.resize(limit - retained);
        }
        retained += events.size();
        for(const auto& e: events) unbounded |= !physicalBounded(e);
        std::stable_sort(events.begin(), events.end(), ReplayLess);
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
        auto full = source.fetchPhysical(story, range, false);
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
    for(const auto& record: records)
    {
        if(record.end <= scan.start || record.start >= scan.end)
            continue;
        if(record.state == ManifestState::Lost)
            archive_failed = true;
        if(record.state != ManifestState::Published)
            continue;
        auto events = options.archive->readRecord(record, range, limit - retained + 1);
        if(!events.ok())
            archive_failed = true;
        else
            take(*std::move(events));
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
class TailStream final: public ReplayStream
{
    struct Source
    {
        std::string instance;
        // [asked from, covered) is final for this source and held in `events`; the next ask starts at `covered`.
        Hlc covered;
        std::vector<Event> events;
    };

public:
    TailStream(std::shared_ptr<const HotSource> source, StoryId story, Event position, HotReplayOptions options)
        : source_(std::move(source))
        , story_(story)
        , options_(std::move(options))
        , position_(std::move(position))
        , frontier_(position_.hlc)
    {
        options_.batch_size = std::max<size_t>(options_.batch_size, 1);
        options_.read_max_events = std::max<size_t>(options_.read_max_events, 2);
    }

    // Polls once at open time so NOT_FOUND surfaces from tail().
    absl::Status open()
    {
        auto fetched = source_->fetchTail(story_, frontier_, {});
        if(!fetched.ok())
            return fetched.status();
        absorb(*fetched);
        return absl::OkStatus();
    }

    absl::StatusOr<std::optional<ReplayBatch>> next() override
    {
        std::unique_lock lk(mu_);
        for(;;)
        {
            if(cancelled_)
                return finish();
            if(pending_pos_ < pending_.size())
                return takeBatch();
            if(suspect_)
            {
                // Only the Catalog says a story is destroyed; a Keeper's refusal or an archive tombstone is evidence.
                suspect_ = false;
                lk.unlock();
                const absl::Status live = options_.story_live ? options_.story_live(story_) : absl::OkStatus();
                lk.lock();
                if(cancelled_)
                    continue;
                if(absl::IsFailedPrecondition(live))
                    return live;
            }
            if(closed_)
                return finish();
            if(idle_)
            {
                cv_.wait_for(lk, options_.tail_poll, [&] { return cancelled_; });
                idle_ = false;
                continue;
            }
            const Hlc from = frontier_;
            TailStarts starts;
            for(const auto& [id, source]: sources_) starts[id] = std::max(source.covered, from);
            lk.unlock();
            auto fetched = source_->fetchTail(story_, from, starts);
            lk.lock();
            if(cancelled_)
                continue;
            if(!fetched.ok())
                return fetched.status();
            idle_ = !absorb(*fetched);
        }
    }

    void cancel() override
    {
        {
            std::lock_guard lk(mu_);
            cancelled_ = true;
        }
        cv_.notify_all();
    }

private:
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
    KeeperFrontier takeReply(KeeperFetch& reply, Epoch route_epoch)
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
            if(ReplayLess(position_, e))
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
        if(fresh.size() > room)
        {
            covered = std::max(from, std::min(covered, fresh[room].hlc));
            std::erase_if(fresh, [&](const Event& e) { return e.hlc >= covered; });
        }
        source.events.insert(source.events.end(),
                             std::make_move_iterator(fresh.begin()),
                             std::make_move_iterator(fresh.end()));
        source.covered = covered;
        f.sealed = covered;
        return f;
    }

    // Returns true when events were queued.
    bool absorb(HotFetch& fetch)
    {
        if(fetch.closed)
            closed_ = true;
        suspect_ = std::any_of(fetch.keepers.begin(),
                               fetch.keepers.end(),
                               [](const KeeperFetch& k)
                               { return k.frontier.status == absl::StatusCode::kFailedPrecondition; }) ||
                   archiveRecordsTombstone();
        if(reachedAbandoned(fetch.abandoned))
        {
            fail(IncompleteReason::SourceFailed);
            return false;
        }
        std::vector<KeeperFrontier> frontiers;
        std::set<SourceId> asked;
        for(auto& k: fetch.keepers)
        {
            asked.insert(SourceId{k.frontier.process_id, k.frontier.predecessor ? k.frontier.expected_epoch : Epoch{}});
            frontiers.push_back(takeReply(k, fetch.route_epoch));
        }
        // I6.13: every source has answered below `bound` and no abandoned range lies there; a Route with no Keeper
        // proves nothing.
        const Range range{Range::Axis::Hlc, frontier_, maxHlc()};
        Hlc bound = prefixCut(range, maxHlc(), fetch.route_epoch, frontiers, fetch.abandoned);
        bound = bound == maxHlc() ? frontier_ : std::max(bound, frontier_);
        std::vector<Event> cold;
        if(!loadArchive(options_, story_, Range{Range::Axis::Hlc, frontier_, bound}, fetch, cold))
        {
            fail(IncompleteReason::SourceFailed);
            return false;
        }
        std::erase_if(cold,
                      [&](const Event& e) { return e.hlc < frontier_ || e.hlc >= bound || !ReplayLess(position_, e); });
        std::vector<std::vector<Event>> inputs;
        inputs.push_back(std::move(cold));
        for(auto it = sources_.begin(); it != sources_.end();)
        {
            auto& held = it->second.events;
            auto split =
                    std::lower_bound(held.begin(), held.end(), bound, [](const Event& e, Hlc h) { return e.hlc < h; });
            inputs.emplace_back(std::make_move_iterator(held.begin()), std::make_move_iterator(split));
            held.erase(held.begin(), split);
            it = held.empty() && !asked.contains(it->first) ? sources_.erase(it) : std::next(it);
        }
        auto events = mergeReplay(std::move(inputs));
        frontier_ = bound;
        if(reachedAbandoned(fetch.abandoned))
            fail(IncompleteReason::SourceFailed);
        if(events.empty())
            return false;
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
        size_t n = std::min(options_.batch_size, pending_.size() - pending_pos_);
        batch.events.assign(std::make_move_iterator(pending_.begin() + pending_pos_),
                            std::make_move_iterator(pending_.begin() + pending_pos_ + n));
        pending_pos_ += n;
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
    std::mutex mu_;
    std::condition_variable cv_;
    // Every event below this has been delivered or precedes the position.
    Hlc frontier_;
    std::map<SourceId, Source> sources_;
    std::vector<Event> pending_;
    size_t pending_pos_{};
    bool cancelled_{};
    bool closed_{};
    bool idle_{};
    bool suspect_{};
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
    auto fetched = range.axis == Range::Axis::Physical ? source_->fetchPhysical(id, range, archive_policy)
                                                       : source_->fetch(id, range);
    if(!fetched.ok())
        return fetched.status();
    if(range.axis == Range::Axis::Physical)
    {
        fetched->physical_policy &= archive_policy;
        return physicalRead(id, range, *fetched, options_, *source_);
    }
    const size_t limit = std::max<size_t>(1, options_.read_max_events);
    Range covered = range;
    bool limited = false;
    std::vector<KeeperFrontier> frontiers;
    std::vector<Hlc> hot_times;
    for(auto& k: fetched->keepers)
    {
        if(k.frontier.truncated && !k.frontier.truncated_at)
            k.frontier.truncated_at = k.events.empty() ? range.start : k.events.back().hlc;
        frontiers.push_back(k.frontier);
        std::erase_if(k.events, [&](const Event& e) { return !inRange(range, e); });
        for(const auto& e: k.events)
            hot_times.push_back(range.axis == Range::Axis::Hlc ? e.hlc : Hlc{e.physical.physical_ns, 0});
    }
    if(hot_times.size() > limit)
    {
        std::nth_element(hot_times.begin(), hot_times.begin() + limit, hot_times.end());
        covered.end = hot_times[limit];
        limited = true;
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
            if(range.axis == Range::Axis::Hlc &&
               (record.end <= range.start || record.start >= boundary || record.start >= covered.end))
                continue;
            published.push_back(record);
        }
        size_t archived_count = 0;
        Hlc accepted_cut = range.start;
        for(size_t i = 0; i < published.size();)
        {
            size_t j = i;
            size_t group_count = 0;
            while(j < published.size() && published[j].start == published[i].start)
                group_count += published[j++].event_count;
            Hlc candidate_cut = j < published.size() ? std::min(covered.end, published[j].start) : covered.end;
            candidate_cut = std::max(range.start, candidate_cut);
            size_t hot_count = hotCount(candidate_cut);
            bool fits = archived_count <= limit && group_count <= limit - archived_count &&
                        hot_count <= limit - archived_count - group_count;
            bool oversized_file = selected.empty() && j == i + 1 && group_count > limit;
            if(!fits && !oversized_file)
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
                       record.end > covered.end)
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
        std::erase_if(k.events, [&](const Event& e) { return !inRange(covered, e); });
        for(const auto& e: k.events) unbounded_event |= !physicalBounded(e);
        std::stable_sort(k.events.begin(), k.events.end(), ReplayLess);
        inputs.push_back(std::move(k.events));
    }
    Range cold{range.axis, range.start, std::min(boundary, covered.end)};
    if(cold.start < cold.end && archive_ok && options_.archive)
    {
        for(const auto& record: records)
            if(record.state == ManifestState::Lost &&
               (range.axis == Range::Axis::Physical || (record.start < cold.end && record.end > cold.start)))
                archive_ok = false;
        for(const auto& record: selected)
        {
            if(range.axis == Range::Axis::Hlc && record.start >= cold.end)
                continue;
            auto events = options_.archive->readRecord(
                    record,
                    range.axis == Range::Axis::Physical ? Range{Range::Axis::Hlc, record.start, record.end} : cold);
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
    if(position.id.story_id != id)
        return absl::InvalidArgumentError("position belongs to a different story");
    auto stream = std::make_unique<TailStream>(source_, id, std::move(position), options_);
    if(auto opened = stream->open(); !opened.ok())
        return opened;
    return std::unique_ptr<ReplayStream>(std::move(stream));
}

} // namespace chronolog::player
