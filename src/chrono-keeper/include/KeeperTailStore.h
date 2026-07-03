#ifndef KEEPER_TAIL_STORE_H
#define KEEPER_TAIL_STORE_H

#include <map>
#include <mutex>
#include <vector>
#include <unordered_map>

#include <chronolog_types.h>
#include <StoryChunk.h>

#include "StoryChunkExtractionQueue.h"

namespace chronolog
{

// KeeperTailStore maintains, per story, a sorted "tail" of the most recent
// events that have rolled out of the active ingestion timeline (i.e. events
// from decayed/sealed StoryChunks). It serves low-latency "playback the last N
// events" queries directly from keeper memory without going through the
// player/HDF5 archive.
//
// Memory model (single payload copy, no duplication):
//   - When a StoryChunk seals, the pipeline hands its OWNERSHIP to the
//     TailStore (instead of stashing it straight to the extraction queue).
//   - The per-story sorted index maps EventSequence -> StoryChunk*; the actual
//     payload lives exactly once, inside the retained chunk's logEvents map.
//   - The tail is capped at `tailCapacity` events per story. When events age
//     out, and a retained chunk has no remaining indexed events, that chunk is
//     finally forwarded to the extraction queue for archival + deletion.
// So a chunk referenced by the tail is "sealed but not yet extracted", and the
// index only ever points at payloads that are still resident in the keeper.
class KeeperTailStore
{
public:
    KeeperTailStore(StoryChunkExtractionQueue &extraction_queue, std::size_t tail_capacity)
        : theExtractionQueue(extraction_queue)
        , tailCapacity(tail_capacity)
    {}

    ~KeeperTailStore()
    {
        // Forward any still-retained chunks to the extraction queue so their
        // data is archived rather than silently dropped on shutdown.
        std::lock_guard <std::mutex> lock(tailMutex);
        for(auto &story_entry: storyTails)
        {
            for(auto &chunk_entry: story_entry.second.liveCounts)
            { theExtractionQueue.stashStoryChunk(chunk_entry.first); }
            story_entry.second.liveCounts.clear();
            story_entry.second.index.clear();
        }
    }

    // Take ownership of a freshly sealed chunk and fold its events into the tail.
    void ingestSealedChunk(StoryId const &story_id, StoryChunk *sealed_chunk)
    {
        if(sealed_chunk == nullptr || sealed_chunk->empty())
        {
            if(sealed_chunk)
            { delete sealed_chunk; }
            return;
        }
        std::lock_guard <std::mutex> lock(tailMutex);
        StoryTail &tail = storyTails[story_id];
        // Key retention on the chunk pointer (unique per live chunk) so accounting
        // never depends on start-time uniqueness among retained chunks.
        tail.liveCounts.emplace(sealed_chunk, (std::size_t)sealed_chunk->getEventCount());
        for(auto it = sealed_chunk->begin(); it != sealed_chunk->end(); ++it)
        { tail.index[it->first] = sealed_chunk; }
        enforceCapacity(tail);
    }

    // Phase 1: this keeper's most recent (up to n) EventSequences for the story.
    std::vector <EventSequence> getTailSequences(StoryId const &story_id, std::size_t n)
    {
        std::vector <EventSequence> result;
        std::lock_guard <std::mutex> lock(tailMutex);
        auto story_it = storyTails.find(story_id);
        if(story_it == storyTails.end())
        { return result; }
        auto &index = story_it->second.index;
        std::size_t take = (n < index.size()) ? n : index.size();
        result.reserve(take);
        // walk from the newest (largest) key backwards, collect `take` of them
        std::size_t skip = index.size() - take;
        auto it = index.begin();
        std::advance(it, skip);
        for(; it != index.end(); ++it)
        { result.push_back(it->first); }
        return result; // ascending order
    }

    // Phase 2: payloads for the requested EventSequences this keeper still holds.
    std::vector <LogEvent> getTailEvents(StoryId const &story_id, std::vector <EventSequence> const &seqs)
    {
        std::vector <LogEvent> result;
        std::lock_guard <std::mutex> lock(tailMutex);
        auto story_it = storyTails.find(story_id);
        if(story_it == storyTails.end())
        { return result; }
        auto &index = story_it->second.index;
        result.reserve(seqs.size());
        for(auto const &seq: seqs)
        {
            auto idx_it = index.find(seq);
            if(idx_it == index.end())
            { continue; }
            LogEvent const *event = idx_it->second->findEvent(seq);
            if(event != nullptr)
            { result.push_back(*event); }
        }
        return result;
    }

private:
    struct StoryTail
    {
        // sorted tail; the payload lives once inside the chunk pointed to.
        std::map <EventSequence, StoryChunk *> index;
        // owns the retained chunks: chunk -> number of its events still in `index`.
        // Keyed by the chunk pointer (unique per live chunk); a chunk is forwarded
        // to the extraction queue and dropped once its count reaches 0.
        std::unordered_map <StoryChunk *, std::size_t> liveCounts;
    };

    // Evict oldest events until the tail is within capacity; once a retained
    // chunk has no indexed events left, forward it to the extraction queue.
    void enforceCapacity(StoryTail &tail)
    {
        while(tail.index.size() > tailCapacity)
        {
            auto oldest = tail.index.begin();
            StoryChunk *chunk = oldest->second;
            tail.index.erase(oldest);
            auto lc = tail.liveCounts.find(chunk);
            if(lc != tail.liveCounts.end() && --lc->second == 0)
            {
                theExtractionQueue.stashStoryChunk(chunk); // archive + free downstream
                tail.liveCounts.erase(lc);
            }
        }
    }

    StoryChunkExtractionQueue &theExtractionQueue;
    std::size_t tailCapacity;
    std::mutex tailMutex;
    std::unordered_map <StoryId, StoryTail> storyTails;
};

} // namespace chronolog

#endif
