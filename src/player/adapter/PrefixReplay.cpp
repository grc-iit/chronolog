#include "player/adapter/PrefixReplay.h"
#include <algorithm>
#include <atomic>
#include <set>
#include "player/adapter/EventConvert.h"
#include "chronolog/message_limits.h"

namespace chronolog::player
{
namespace
{

// A finished result served in batches: the events are already merged, deduplicated and in the order of I7.7.
class MergedStream final: public ReplayStream
{
public:
    MergedStream(std::vector<Event> events, Completion completion, size_t batch_size)
        : events_(std::move(events))
        , completion_(std::move(completion))
        , batch_size_(std::max<size_t>(batch_size, 1))
    {}

    absl::StatusOr<std::optional<ReplayBatch>> next() override
    {
        if(cancelled_.load())
            return absl::CancelledError("replay stream cancelled");
        ReplayBatch batch;
        size_t bytes = 0;
        while(pos_ < events_.size() && batch.events.size() < batch_size_)
        {
            const size_t event_bytes = convert::encodedSize(events_[pos_]);
            if(!batch.events.empty() && bytes + event_bytes > kEventBatchBytes)
                break;
            bytes += event_bytes;
            batch.events.push_back(std::move(events_[pos_++]));
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
    std::vector<Event> events_;
    Completion completion_;
    size_t batch_size_;
    size_t pos_{};
    bool completion_sent_{};
    std::atomic<bool> cancelled_{false};
};

int rank(IncompleteReason reason)
{
    switch(reason)
    {
        case IncompleteReason::SourceFailed:
            return 4;
        case IncompleteReason::Truncated:
            return 3;
        case IncompleteReason::PhysicalAxisUnbounded:
            return 2;
        case IncompleteReason::LaggingWriters:
            return 1;
        default:
            return 0;
    }
}

// What reading every story of one resolved set produced.
struct SetRead
{
    std::vector<Event> events;
    Completion completion;
    // Stories whose own Read failed because they are destroyed or unknown.
    bool story_gone{};
};

bool destroyedStory(const absl::Status& status)
{
    return absl::IsNotFound(status) || absl::IsFailedPrecondition(status);
}

} // namespace

absl::StatusOr<std::unique_ptr<ReplayStream>>
PrefixReplay::read(const std::string& prefix, Range range, size_t max_events, const EventPredicate& predicate) const
{
    if(auto valid = predicate.validate(); !valid.ok())
        return valid;
    if(range.end < range.start)
        return absl::InvalidArgumentError("range end precedes start");
    const size_t limit = std::max<size_t>(1, max_events ? max_events : options_.read_max_events);
    const bool hlc_axis = range.axis == Range::Axis::Hlc;

    auto readSet = [&](const PrefixResolution& set) -> absl::StatusOr<SetRead>
    {
        SetRead out;
        out.completion.complete = true;
        std::optional<Hlc> cut;
        std::optional<Hlc> frontier;
        for(StoryId story: set.stories)
        {
            auto opened = replay_.read(story, range, limit, predicate);
            if(!opened.ok() && destroyedStory(opened.status()))
            {
                out.story_gone = true;
                continue;
            }
            if(!opened.ok())
                return opened.status();
            std::vector<Event> events;
            std::optional<Completion> completion;
            for(;;)
            {
                auto next = (*opened)->next();
                if(!next.ok())
                    return next.status();
                if(!*next)
                    break;
                events.insert(events.end(),
                              std::make_move_iterator((*next)->events.begin()),
                              std::make_move_iterator((*next)->events.end()));
                if((*next)->completion)
                    completion = std::move((*next)->completion);
            }
            if(!completion)
                return absl::InternalError("story Read ended without a Completion");
            out.completion.complete &= completion->complete;
            if(rank(completion->reason) > rank(out.completion.reason))
                out.completion.reason = completion->reason;
            out.completion.laggards.insert(out.completion.laggards.end(),
                                           completion->laggards.begin(),
                                           completion->laggards.end());
            frontier = frontier ? std::min(*frontier, completion->frontier) : completion->frontier;
            if(cut)
                std::erase_if(events, [&](const Event& e) { return e.hlc >= *cut; });
            const auto middle = static_cast<std::ptrdiff_t>(out.events.size());
            out.events.insert(out.events.end(),
                              std::make_move_iterator(events.begin()),
                              std::make_move_iterator(events.end()));
            std::inplace_merge(out.events.begin(), out.events.begin() + middle, out.events.end(), PrefixLess);
            // The merged stream holds the target, whole HLC tie groups at a time, and what lies past it is cut.
            if(hlc_axis && out.events.size() > limit)
            {
                const Hlc last = out.events[limit - 1].hlc;
                auto past = std::upper_bound(out.events.begin(),
                                             out.events.end(),
                                             last,
                                             [](Hlc h, const Event& e) { return h < e.hlc; });
                if(past != out.events.end())
                {
                    cut = cut ? std::min(*cut, past->hlc) : past->hlc;
                    out.events.erase(past, out.events.end());
                }
            }
        }
        out.completion.frontier = frontier.value_or(Hlc{});
        if(cut)
        {
            out.completion.complete = false;
            if(rank(out.completion.reason) < rank(IncompleteReason::Truncated))
                out.completion.reason = IncompleteReason::Truncated;
            out.completion.frontier = std::min(out.completion.frontier, *cut);
        }
        return out;
    };

    auto resolved = catalog_.resolvePrefix(prefix, options_.max_scopes);
    if(!resolved.ok())
        return resolved.status();
    for(uint32_t attempt = 0;; ++attempt)
    {
        if(resolved->stories.empty())
        {
            // No Keeper proved a frontier for a prefix that matches no story, so there is no claim (I6.15).
            Completion none{false, Hlc{}, {}, IncompleteReason::LaggingWriters, resolved->revision};
            return std::unique_ptr<ReplayStream>(
                    std::make_unique<MergedStream>(std::vector<Event>{}, std::move(none), options_.batch_size));
        }
        auto pass = readSet(*resolved);
        if(!pass.ok())
            return pass.status();
        // I6.16: after the last frontier, one more linearizable look at the Catalog.
        auto confirmed = catalog_.confirmPrefix(prefix, options_.max_scopes, *resolved);
        if(!confirmed.ok())
            return confirmed.status();
        if(!confirmed->created || attempt == options_.resolve_retries)
        {
            Completion& completion = pass->completion;
            completion.catalog_revision = resolved->revision;
            // A story of the set destroyed while the Read ran ends it SOURCE_FAILED and leaves no event of its own (I6.15).
            const std::set<StoryId> alive(confirmed->current.stories.begin(), confirmed->current.stories.end());
            std::set<StoryId> destroyed;
            for(StoryId story: resolved->stories)
                if(!alive.contains(story))
                    destroyed.insert(story);
            if(pass->story_gone || !destroyed.empty())
            {
                completion.complete = false;
                completion.reason = IncompleteReason::SourceFailed;
                std::erase_if(pass->events, [&](const Event& e) { return destroyed.contains(e.id.story_id); });
            }
            else if(confirmed->created)
            {
                completion.complete = false;
                if(rank(completion.reason) < rank(IncompleteReason::LaggingWriters))
                    completion.reason = IncompleteReason::LaggingWriters;
            }
            // I6.15: a TRUNCATED prefix Read holds no event of any story at or above c.
            if(completion.reason == IncompleteReason::Truncated && hlc_axis)
                std::erase_if(pass->events, [&](const Event& e) { return e.hlc >= completion.frontier; });
            return std::unique_ptr<ReplayStream>(std::make_unique<MergedStream>(std::move(pass->events),
                                                                                std::move(completion),
                                                                                options_.batch_size));
        }
        resolved = std::move(confirmed->current);
    }
}

absl::StatusOr<std::unique_ptr<ReplayStream>>
PrefixReplay::tail(const std::string& prefix, Event position, const EventPredicate& predicate) const
{
    auto resolved = catalog_.resolvePrefix(prefix, options_.max_scopes);
    if(!resolved.ok())
        return resolved.status();
    return replay_.tail(std::move(resolved->stories), resolved->revision, std::move(position), predicate);
}

} // namespace chronolog::player
