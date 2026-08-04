#ifndef KEEPER_TAIL_STORE_H
#define KEEPER_TAIL_STORE_H

#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <unordered_map>

#include <chronolog_types.h>
#include <StoryChunk.h>

#include "ActiveTailSource.h"
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
    // tail_retention_ns bounds how long a sealed chunk may sit in the tail before it
    // is forwarded for archival. Without it the ONLY forwarding paths are capacity
    // eviction and shutdown, which makes archival volume-driven: a story producing
    // fewer than tail_capacity events never reaches the grapher while the keeper
    // runs, so its data is never written to HDF5 and survives only in keeper RAM.
    // Before the tail store existed the pipeline handed each decayed chunk straight
    // to the extraction queue; this restores that time-driven guarantee.
    KeeperTailStore(StoryChunkExtractionQueue& extraction_queue,
                    std::size_t tail_capacity,
                    bool live_tail_read = false,
                    uint64_t tail_retention_ns = 0)
        : theExtractionQueue(extraction_queue)
        , tailCapacity(tail_capacity)
        , liveTailRead(live_tail_read)
        , tailRetentionNs(tail_retention_ns)
    {}

    ~KeeperTailStore()
    {
        // Forward any still-retained chunks to the extraction queue so their
        // data is archived rather than silently dropped on shutdown.
        std::lock_guard<std::mutex> lock(tailMutex);
        for(auto& story_entry: storyTails)
        {
            for(auto& chunk_entry: story_entry.second.liveCounts)
            {
                theExtractionQueue.stashStoryChunk(chunk_entry.first);
            }
            story_entry.second.liveCounts.clear();
            story_entry.second.index.clear();
        }
    }

    // Take ownership of a freshly sealed chunk and fold its events into the tail.
    void ingestSealedChunk(StoryId const& story_id, StoryChunk* sealed_chunk)
    {
        if(sealed_chunk == nullptr || sealed_chunk->empty())
        {
            if(sealed_chunk)
            {
                delete sealed_chunk;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(tailMutex);
        StoryTail& tail = storyTails[story_id];
        // Key retention on the chunk pointer (unique per live chunk) so accounting
        // never depends on start-time uniqueness among retained chunks.
        tail.liveCounts.emplace(sealed_chunk, (std::size_t)sealed_chunk->getEventCount());
        for(auto it = sealed_chunk->begin(); it != sealed_chunk->end(); ++it) { tail.index[it->first] = sealed_chunk; }
        enforceCapacity(tail);
    }

    // Forward for archival every retained chunk whose retention window has passed,
    // so a story that never fills the tail is still archived. Called on the keeper's
    // maintenance tick with the same current_time that drives chunk decay, so this
    // shares the pipeline's clock basis rather than introducing a second one.
    //
    // A chunk is compared on its END time: the whole chunk becomes archivable at
    // once, which is the granularity the extraction queue takes ownership at.
    // tail_retention_ns == 0 disables age-out (capacity/shutdown only).
    void ageOutChunks(uint64_t current_time)
    {
        if(tailRetentionNs == 0)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(tailMutex);
        for(auto& story_entry: storyTails)
        {
            StoryTail& tail = story_entry.second;
            // index is ordered by EventSequence (event time first) and chunks are
            // time-partitioned, so the oldest entry belongs to the oldest chunk;
            // stop at the first chunk still inside its window.
            while(!tail.index.empty())
            {
                StoryChunk const* oldest_chunk = tail.index.begin()->second;
                if(oldest_chunk->getEndTime() + tailRetentionNs > current_time)
                {
                    break;
                }
                releaseOldestIndexedEvent(tail);
            }
        }
    }

    // Register/unregister the active (unsealed) timeline source for a story. Only
    // consulted when live_tail_read is enabled. The pipeline registers itself on
    // creation and unregisters on retirement so the tail read never dereferences
    // a freed pipeline (see the activeSourcesMutex hold in the query paths).
    void registerActiveSource(StoryId const& story_id, ActiveTailSource* source)
    {
        if(source == nullptr)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(activeSourcesMutex);
        activeSources[story_id] = source;
    }

    void unregisterActiveSource(StoryId const& story_id, ActiveTailSource* source)
    {
        std::lock_guard<std::mutex> lock(activeSourcesMutex);
        auto it = activeSources.find(story_id);
        if(it != activeSources.end() && it->second == source)
        {
            activeSources.erase(it);
        }
    }

    // Phase 1: this keeper's most recent (up to n) EventSequences for the story.
    // With live_tail_read enabled, the sealed-tail last-N is unioned with the
    // active (unsealed) timeline's last-N, and the global last-N is returned.
    std::vector<EventSequence> getTailSequences(StoryId const& story_id, std::size_t n)
    {
        std::vector<EventSequence> sealed = getSealedTailSequences(story_id, n);

        if(!liveTailRead)
        {
            return sealed; // already ascending, capped at n
        }

        std::vector<EventSequence> active = getActiveTailSequences(story_id, n);
        if(active.empty())
        {
            return sealed;
        }

        // Union sealed + active, dedup, keep the globally most-recent n (ascending).
        std::set<EventSequence> merged(sealed.begin(), sealed.end());
        merged.insert(active.begin(), active.end());
        std::size_t take = (n < merged.size()) ? n : merged.size();
        std::vector<EventSequence> result;
        result.reserve(take);
        auto it = merged.end();
        for(std::size_t i = 0; i < take; ++i) { --it; }
        for(; it != merged.end(); ++it) { result.push_back(*it); }
        return result; // ascending order
    }

    // Phase 2: payloads for the requested EventSequences this keeper still holds.
    // Sealed-tail hits are served first; any seqs not in the sealed tail are, when
    // live_tail_read is enabled, looked up in the active timeline (e.g. events
    // whose sequence phase-1 returned from the open chunk).
    std::vector<LogEvent> getTailEvents(StoryId const& story_id, std::vector<EventSequence> const& seqs)
    {
        std::vector<LogEvent> result;
        result.reserve(seqs.size());
        std::vector<EventSequence> misses;

        {
            std::lock_guard<std::mutex> lock(tailMutex);
            auto story_it = storyTails.find(story_id);
            auto* index = (story_it == storyTails.end()) ? nullptr : &story_it->second.index;
            for(auto const& seq: seqs)
            {
                LogEvent const* event = nullptr;
                if(index != nullptr)
                {
                    auto idx_it = index->find(seq);
                    if(idx_it != index->end())
                    {
                        event = idx_it->second->findEvent(seq);
                    }
                }
                if(event != nullptr)
                {
                    result.push_back(*event);
                }
                else if(liveTailRead)
                {
                    misses.push_back(seq);
                }
            }
        }

        // Resolve remaining seqs from the active timeline without holding tailMutex
        // (preserves the pipeline->tail lock order used by the seal/decay path).
        if(liveTailRead && !misses.empty())
        {
            std::lock_guard<std::mutex> lock(activeSourcesMutex);
            auto src_it = activeSources.find(story_id);
            if(src_it != activeSources.end())
            {
                for(auto const& seq: misses)
                {
                    LogEvent event;
                    if(src_it->second->findActiveEvent(seq, event))
                    {
                        result.push_back(event);
                    }
                }
            }
        }
        return result;
    }

private:
    // Sealed-tail last-N (ascending), the historical behavior.
    std::vector<EventSequence> getSealedTailSequences(StoryId const& story_id, std::size_t n)
    {
        std::vector<EventSequence> result;
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyTails.find(story_id);
        if(story_it == storyTails.end())
        {
            return result;
        }
        auto& index = story_it->second.index;
        std::size_t take = (n < index.size()) ? n : index.size();
        result.reserve(take);
        // Position `take` entries back from end() -- O(take) -- rather than
        // advancing O(index.size() - take) forward from begin() (which costs
        // ~tailCapacity node-steps to fetch the last few events of a full tail),
        // then walk forward to end() so the result comes out ascending.
        auto it = index.end();
        for(std::size_t i = 0; i < take; ++i) { --it; }
        for(; it != index.end(); ++it) { result.push_back(it->first); }
        return result; // ascending order
    }

    // Active (unsealed) timeline last-N (ascending) via the registered source.
    // Holds activeSourcesMutex across the call so the pipeline cannot be
    // unregistered/freed mid-lookup.
    std::vector<EventSequence> getActiveTailSequences(StoryId const& story_id, std::size_t n)
    {
        std::lock_guard<std::mutex> lock(activeSourcesMutex);
        auto src_it = activeSources.find(story_id);
        if(src_it == activeSources.end())
        {
            return {};
        }
        return src_it->second->activeTailSequences(n);
    }

    struct StoryTail
    {
        // sorted tail; the payload lives once inside the chunk pointed to.
        std::map<EventSequence, StoryChunk*> index;
        // owns the retained chunks: chunk -> number of its events still in `index`.
        // Keyed by the chunk pointer (unique per live chunk); a chunk is forwarded
        // to the extraction queue and dropped once its count reaches 0.
        std::unordered_map<StoryChunk*, std::size_t> liveCounts;
    };

    // Drop the oldest indexed event; when that was a chunk's last indexed event the
    // chunk has no readers left, so ownership passes to the extraction queue (which
    // archives and frees it). Releasing only on the last reference is what keeps the
    // index from ever pointing at a chunk the drain thread is about to delete.
    void releaseOldestIndexedEvent(StoryTail& tail)
    {
        auto oldest = tail.index.begin();
        StoryChunk* chunk = oldest->second;
        tail.index.erase(oldest);
        auto lc = tail.liveCounts.find(chunk);
        if(lc != tail.liveCounts.end() && --lc->second == 0)
        {
            theExtractionQueue.stashStoryChunk(chunk); // archive + free downstream
            tail.liveCounts.erase(lc);
        }
    }

    // Evict oldest events until the tail is within capacity; once a retained
    // chunk has no indexed events left, forward it to the extraction queue.
    void enforceCapacity(StoryTail& tail)
    {
        while(tail.index.size() > tailCapacity) { releaseOldestIndexedEvent(tail); }
    }

    StoryChunkExtractionQueue& theExtractionQueue;
    std::size_t tailCapacity;
    bool liveTailRead;
    uint64_t tailRetentionNs;
    std::mutex tailMutex;
    std::unordered_map<StoryId, StoryTail> storyTails;

    // Active (unsealed) timeline sources, keyed by story. Guarded by its own
    // mutex, never nested inside tailMutex, to keep the pipeline->tail lock order.
    std::mutex activeSourcesMutex;
    std::unordered_map<StoryId, ActiveTailSource*> activeSources;
};

} // namespace chronolog

#endif
