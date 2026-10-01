#include "chrono-player/replay/HotReplay.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
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
public:
    HotReplayStream(std::vector<Event> events, Completion completion, size_t batch_size)
        : events_(std::move(events))
        , completion_(std::move(completion))
        , batch_size_(std::max<size_t>(batch_size, 1))
    {}

    absl::StatusOr<std::optional<ReplayBatch>> next() override
    {
        if(cancelled_.load())
            return absl::CancelledError("replay stream cancelled");
        ReplayBatch batch;
        if(pos_ < events_.size())
        {
            size_t n = std::min(batch_size_, events_.size() - pos_);
            batch.events.assign(std::make_move_iterator(events_.begin() + pos_),
                                std::make_move_iterator(events_.begin() + pos_ + n));
            pos_ += n;
            return std::optional<ReplayBatch>(std::move(batch));
        }
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
    std::vector<Event> events_;
    Completion completion_;
    size_t batch_size_;
    size_t pos_{};
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
            return false;
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
        batch.completion = Completion{false, cursor_.hlc, {}, IncompleteReason::None};
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
    std::vector<KeeperFrontier> frontiers;
    frontiers.reserve(fetched->keepers.size());
    for(const auto& k: fetched->keepers) frontiers.push_back(k.frontier);
    std::vector<Event> cold;
    bool archive_ok = loadArchive(options_, id, range, *fetched, cold);
    Completion completion =
            CompletionPolicy::decide(range, fetched->route_epoch, frontiers, fetched->writers, !archive_ok);
    auto hot = mergeKept(*fetched, [&](const Event& e) { return inRange(range, e); });
    auto events = mergeReplay({std::move(cold), std::move(hot)});
    return std::unique_ptr<ReplayStream>(
            std::make_unique<HotReplayStream>(std::move(events), std::move(completion), options_.batch_size));
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
