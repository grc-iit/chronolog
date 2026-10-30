#include <iterator>

#include <StoryChunk.h>
#include <WalReleaseTracker.h>

namespace chronolog
{

void WalReleaseTracker::addCopy(StoryId story_id, EventSequence const& event, uint64_t segment, uint64_t bytes)
{
    std::vector<Copy>& copies = pending[story_id][event];
    if(copies.empty())
    {
        ++pendingEventCount;
    }
    copies.push_back(Copy{segment, bytes});
    ++outstandingCopies[segment];
    pendingByteCount += bytes;
}

void WalReleaseTracker::forgetCopies(std::vector<Copy> const& copies)
{
    for(Copy const& copy: copies)
    {
        auto segment = outstandingCopies.find(copy.segment);
        if(segment != outstandingCopies.end() && --segment->second == 0)
        {
            outstandingCopies.erase(segment);
        }
        pendingByteCount -= copy.bytes;
    }
    --pendingEventCount;
}

void WalReleaseTracker::releaseEvent(StoryId story_id, EventSequence const& event)
{
    auto story = pending.find(story_id);
    if(story == pending.end())
    {
        return;
    }
    auto pending_event = story->second.find(event);
    if(pending_event == story->second.end())
    {
        return;
    }
    forgetCopies(pending_event->second);
    story->second.erase(pending_event);
    if(story->second.empty())
    {
        pending.erase(story);
    }
}

void WalReleaseTracker::releaseChunk(StoryId story_id, StoryChunk const& chunk)
{
    auto story = pending.find(story_id);
    if(story == pending.end() || chunk.empty())
    {
        return;
    }
    auto& events = story->second;

    // Walk the smaller side: a chunk of mostly accepted events usually holds
    // far more events than the story has pending, and a story with a deep
    // backlog far more than one chunk.
    if(events.size() <= static_cast<std::size_t>(chunk.getEventCount()))
    {
        auto pending_event = events.lower_bound(chunk.begin()->first);
        auto const last = events.upper_bound(std::prev(chunk.end())->first);
        while(pending_event != last)
        {
            if(chunk.findEvent(pending_event->first) != nullptr)
            {
                forgetCopies(pending_event->second);
                pending_event = events.erase(pending_event);
            }
            else
            {
                ++pending_event;
            }
        }
    }
    else
    {
        for(auto const& chunk_event: chunk)
        {
            auto pending_event = events.find(chunk_event.first);
            if(pending_event != events.end())
            {
                forgetCopies(pending_event->second);
                events.erase(pending_event);
            }
        }
    }

    if(events.empty())
    {
        pending.erase(story);
    }
}

void WalReleaseTracker::dropStory(StoryId story_id)
{
    auto story = pending.find(story_id);
    if(story == pending.end())
    {
        return;
    }
    for(auto const& pending_event: story->second) { forgetCopies(pending_event.second); }
    pending.erase(story);
}

bool WalReleaseTracker::isPending(StoryId story_id, EventSequence const& event) const
{
    auto story = pending.find(story_id);
    return story != pending.end() && story->second.count(event) != 0;
}

std::size_t WalReleaseTracker::pendingEvents(StoryId story_id) const
{
    auto story = pending.find(story_id);
    return (story == pending.end()) ? 0 : story->second.size();
}

std::size_t WalReleaseTracker::pendingEvents() const { return pendingEventCount; }

uint64_t WalReleaseTracker::pendingBytes() const { return pendingByteCount; }

uint64_t WalReleaseTracker::outstanding(uint64_t segment) const
{
    auto found = outstandingCopies.find(segment);
    return (found == outstandingCopies.end()) ? 0 : found->second;
}

std::vector<uint64_t> WalReleaseTracker::deletable(std::vector<uint64_t> const& segments_oldest_first,
                                                   uint64_t active) const
{
    std::vector<uint64_t> segments;
    for(uint64_t segment: segments_oldest_first)
    {
        if(segment == active || outstanding(segment) != 0)
        {
            break;
        }
        segments.push_back(segment);
    }
    return segments;
}

} // namespace chronolog
