#include "chrono-player/replay/HotReplay.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <queue>
#include <set>
#include <tuple>
#include "chrono-player/replay/CompletionPolicy.h"
#include "chrono-player/replay/ReplayMerge.h"

namespace chronolog::player
{
namespace
{

bool inRange(const Range& r, const Event& e)
{
    if(r.axis == Range::Axis::Physical)
        return e.physical.physical_ns >= r.start.physical_ns && e.physical.physical_ns < r.end.physical_ns;
    return e.hlc >= r.start && e.hlc < r.end;
}

bool routeAnswered(const HotFetch& f)
{
    return std::all_of(f.keepers.begin(),
                       f.keepers.end(),
                       [&](const KeeperFetch& k) { return k.frontier.answered && k.frontier.epoch == f.route_epoch; });
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
    Hlc boundary{};
    for(const auto& keeper: fetch.keepers) boundary = std::max(boundary, keeper.frontier.evicted_below);
    if(range.axis == Range::Axis::Physical && boundary > Hlc{})
        return range.end;
    return std::min(range.end, boundary);
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
    return archived.ok() && lost.ok() && !*lost;
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

Hlc maxHlc() { return {std::numeric_limits<int64_t>::max(), std::numeric_limits<uint32_t>::max()}; }

class TailStream final: public ReplayStream
{
public:
    TailStream(std::shared_ptr<const HotSource> source, StoryId story, Event position, HotReplayOptions options)
        : source_(std::move(source))
        , story_(story)
        , options_(options)
        , cursor_(std::move(position))
    {
        options_.batch_size = std::max<size_t>(options_.batch_size, 1);
    }

    // Absorbs an initial poll made at open time so NOT_FOUND surfaces from tail().
    void seed(HotFetch fetch) { absorb(fetch); }

    absl::StatusOr<std::optional<ReplayBatch>> next() override
    {
        std::unique_lock lk(mu_);
        for(;;)
        {
            if(cancelled_)
                return finish();
            if(pending_pos_ < pending_.size())
                return takeBatch();
            if(closed_)
                return finish();
            if(idle_)
            {
                cv_.wait_for(lk, options_.tail_poll, [&] { return cancelled_; });
                idle_ = false;
                continue;
            }
            Range range{Range::Axis::Hlc, cursor_.hlc, maxHlc()};
            lk.unlock();
            auto fetched = source_->fetch(story_, range);
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
    // Returns true when events were queued. A poll where any Route Keeper failed is dropped
    // whole, so the cursor never advances past events a silent Keeper may still hold.
    bool absorb(HotFetch& fetch)
    {
        if(fetch.closed)
            closed_ = true;
        if(!routeAnswered(fetch))
            return false;
        std::vector<Event> cold;
        if(!loadArchive(options_, story_, Range{Range::Axis::Hlc, cursor_.hlc, maxHlc()}, fetch, cold))
        {
            closed_ = true;
            final_reason_ = IncompleteReason::SourceFailed;
            return false;
        }
        std::erase_if(cold, [&](const Event& e) { return !ReplayLess(cursor_, e); });
        auto hot = mergeKept(fetch, [&](const Event& e) { return ReplayLess(cursor_, e); });
        auto events = mergeReplay({std::move(cold), std::move(hot)});
        if(events.empty())
            return false;
        cursor_ = events.back();
        pending_ = std::move(events);
        pending_pos_ = 0;
        return true;
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
        batch.completion = Completion{false, cursor_.hlc, {}, final_reason_};
        return batch;
    }

    const std::shared_ptr<const HotSource> source_;
    const StoryId story_;
    HotReplayOptions options_;
    std::mutex mu_;
    std::condition_variable cv_;
    Event cursor_;
    std::vector<Event> pending_;
    size_t pending_pos_{};
    bool cancelled_{};
    bool closed_{};
    bool idle_{};
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
    auto fetched = source_->fetch(id, range);
    if(!fetched.ok())
        return fetched.status();
    const size_t limit = std::max<size_t>(1, options_.read_max_events);
    Range covered = range;
    bool limited = false;
    std::vector<KeeperFrontier> frontiers;
    std::vector<Hlc> hot_times;
    for(auto& k: fetched->keepers)
    {
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
    std::vector<std::vector<Event>> inputs;
    for(auto& k: fetched->keepers)
    {
        std::erase_if(k.events, [&](const Event& e) { return !inRange(covered, e); });
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
            auto events = options_.archive->readRecord(record, cold);
            if(!events.ok())
                archive_ok = false;
            else
                inputs.push_back(*std::move(events));
        }
    }
    Completion completion =
            CompletionPolicy::decide(covered, fetched->route_epoch, frontiers, fetched->writers, !archive_ok);
    if(limited && completion.reason != IncompleteReason::SourceFailed)
    {
        completion.complete = false;
        completion.reason = IncompleteReason::Truncated;
        completion.frontier = covered.end;
    }
    return std::unique_ptr<ReplayStream>(
            std::make_unique<HotReplayStream>(std::move(inputs), std::move(completion), options_.batch_size));
}

absl::StatusOr<std::unique_ptr<ReplayStream>> HotReplay::tail(StoryId id, Event position) const
{
    if(position.id.story_id != id)
        return absl::InvalidArgumentError("position belongs to a different story");
    auto first = source_->fetch(id, Range{Range::Axis::Hlc, position.hlc, maxHlc()});
    if(!first.ok())
        return first.status();
    auto stream = std::make_unique<TailStream>(source_, id, std::move(position), options_);
    stream->seed(std::move(*first));
    return std::unique_ptr<ReplayStream>(std::move(stream));
}

} // namespace chronolog::player
