#ifndef KEEPER_CHUNK_RETENTION_STORE_H
#define KEEPER_CHUNK_RETENTION_STORE_H

#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include <unordered_map>

#include <chronolog_types.h>
#include <StoryChunk.h>

#include "StoryChunkExtractionQueue.h"

namespace chronolog
{

// KeeperChunkRetentionStore owns every sealed StoryChunk on the keeper, from
// the moment the pipeline seals it until its events are confirmed durable.
// It unifies two retention reasons behind one owner:
//
//   - the last-N tail (per-story sorted index over recent events) that serves
//     low-latency playback() tail reads straight from keeper memory, and
//   - durability-gated retention: a chunk is shipped to the grapher on seal
//     (its pointer is stashed to the extraction queue immediately), but the
//     payload is retained until the grapher's persisted watermark W covers it.
//
// Single free condition, checked by one helper from every mutating path:
//
//   shipped (grapher acked) AND chunk.endTime <= known W (report received)
//   AND tail released (no indexed events left) AND not sitting in the
//   extraction queue (a re-send in flight must never dangle).
//
// Stale watermark views cause only extra retention or redundant re-sends
// (deduplicated by the grapher / on read), never data loss: E <= W.
//
// Locking: one mutex guards every structure; drain-thread callbacks
// (markShipped/markSendFailed), watermark reports (confirmPersisted), the
// stall timer (requeueStalled), tail reads and range fetches all serialize
// here. Queue stashes happen outside the lock.
//
// Shutdown contract: destroy this store only after the extraction module has
// been shut down (the keeper main enforces this order). The destructor hands
// still-queued or unshipped chunks to the queue — whose own shutdown frees
// them — and logs every range that was never confirmed persisted.
class KeeperChunkRetentionStore
{
public:
    KeeperChunkRetentionStore(StoryChunkExtractionQueue& extraction_queue, std::size_t tail_capacity,
                              std::size_t retention_cap_mb = 0)
        : theExtractionQueue(extraction_queue)
        , tailCapacity(tail_capacity)
        , retentionCapBytes(retention_cap_mb * 1024 * 1024)
    {}

    ~KeeperChunkRetentionStore()
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        for(auto& story_entry: storyRetention)
        {
            for(auto& chunk_entry: story_entry.second.chunks)
            {
                StoryChunk* chunk = chunk_entry.first;
                ChunkState& state = chunk_entry.second;
                bool const durable = state.shipped && (chunk->getEndTime() <= story_entry.second.known_w);
                if(!durable)
                {
                    LOG_WARNING("[KeeperChunkRetentionStore] shutdown with unconfirmed StoryId={} chunk {}-{} "
                                "eventCount {} (shipped={}, known_W={})",
                                story_entry.first,
                                chunk->getStartTime(),
                                chunk->getEndTime(),
                                chunk->getEventCount(),
                                state.shipped,
                                story_entry.second.known_w);
                }
                if(state.in_queue)
                {
                    // the queue already holds this pointer and frees it in its
                    // own shutdown — freeing here would double-free
                    continue;
                }
                if(!state.shipped)
                {
                    // last-chance handoff for archival (matches the historic
                    // TailStore shutdown behavior); the queue frees it
                    theExtractionQueue.stashStoryChunk(chunk);
                }
                else
                {
                    delete chunk;
                }
            }
            story_entry.second.chunks.clear();
            story_entry.second.index.clear();
        }
    }

    // Take ownership of a freshly sealed chunk: fold its events into the tail
    // index AND stash the pointer to the extraction queue (ship-on-seal).
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
        {
            std::lock_guard<std::mutex> lock(tailMutex);
            StoryRetention& story = storyRetention[story_id];
            ChunkState state;
            state.indexed_count = (std::size_t)sealed_chunk->getEventCount();
            state.approx_bytes = approxChunkBytes(sealed_chunk);
            state.in_queue = true;
            state.last_activity = std::chrono::steady_clock::now();
            story.chunks.emplace(sealed_chunk, state);
            for(auto it = sealed_chunk->begin(); it != sealed_chunk->end(); ++it)
            {
                story.index[it->first] = sealed_chunk;
            }
            retainedBytes += state.approx_bytes;
            LOG_INFO("[KeeperChunkRetentionStore] retaining StoryId={} chunk {}-{} eventCount {} (retained_chunks={})",
                     story_id,
                     sealed_chunk->getStartTime(),
                     sealed_chunk->getEndTime(),
                     sealed_chunk->getEventCount(),
                     story.chunks.size());
            enforceCapacity(story);
            warnOnCapCrossing();
        }
        theExtractionQueue.stashStoryChunk(sealed_chunk);
    }

    // Drain callback: the chunk left the extraction queue and every extractor
    // accepted it (grapher ack). Never frees by itself unless the free
    // condition already holds (tail released and W already past it).
    void markShipped(StoryChunk* chunk)
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(chunk->getStoryId());
        auto chunk_it = (story_it == storyRetention.end()) ? decltype(story_it->second.chunks.begin())()
                                                           : story_it->second.chunks.find(chunk);
        if(story_it == storyRetention.end() || chunk_it == story_it->second.chunks.end())
        {
            LOG_WARNING("[KeeperChunkRetentionStore] markShipped for untracked chunk StoryId={} {}-{}; deleting",
                        chunk->getStoryId(),
                        chunk->getStartTime(),
                        chunk->getEndTime());
            delete chunk;
            return;
        }
        chunk_it->second.shipped = true;
        chunk_it->second.in_queue = false;
        chunk_it->second.last_activity = std::chrono::steady_clock::now();
        maybeFreeChunk(story_it->second, chunk_it);
    }

    // Drain callback: transfer failed. The chunk stays retained and readable;
    // the stall timer (requeueStalled) re-sends it later — no immediate
    // re-stash, which would busy-loop against a dead grapher.
    void markSendFailed(StoryChunk* chunk)
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(chunk->getStoryId());
        auto chunk_it = (story_it == storyRetention.end()) ? decltype(story_it->second.chunks.begin())()
                                                           : story_it->second.chunks.find(chunk);
        if(story_it == storyRetention.end() || chunk_it == story_it->second.chunks.end())
        {
            LOG_WARNING("[KeeperChunkRetentionStore] markSendFailed for untracked chunk StoryId={} {}-{}; deleting",
                        chunk->getStoryId(),
                        chunk->getStartTime(),
                        chunk->getEndTime());
            delete chunk;
            return;
        }
        chunk_it->second.in_queue = false;
        chunk_it->second.last_activity = std::chrono::steady_clock::now();
        LOG_WARNING("[KeeperChunkRetentionStore] send failed for StoryId={} chunk {}-{}; retained for re-send",
                    chunk->getStoryId(),
                    chunk->getStartTime(),
                    chunk->getEndTime());
        // a re-send of an already-shipped chunk may fail without demoting it:
        // the first ack still stands; W is what it waits for
        maybeFreeChunk(story_it->second, chunk_it);
    }

    // Watermark report: everything below w for this story is durable in the
    // archive. Records max(known_w, w) and frees every chunk meeting the free
    // condition. A regression (w below known_w) frees nothing and is logged.
    void confirmPersisted(StoryId const& story_id, uint64_t w)
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        StoryRetention& story = storyRetention[story_id];
        if(w < story.known_w)
        {
            LOG_WARNING("[KeeperChunkRetentionStore] StoryId={} watermark regression {} < known {} ignored",
                        story_id,
                        w,
                        story.known_w);
            return;
        }
        story.known_w = w;
        for(auto chunk_iter = story.chunks.begin(); chunk_iter != story.chunks.end();)
        {
            chunk_iter = maybeFreeChunk(story, chunk_iter);
        }
    }

    uint64_t knownPersisted(StoryId const& story_id) const
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(story_id);
        return (story_it == storyRetention.end()) ? 0 : story_it->second.known_w;
    }

    // Stall timer: re-stash chunks that are not already queued, are not
    // covered by the known watermark, and have seen no activity for max_age.
    // Covers both send-failed chunks and shipped ones whose ack or watermark
    // report was lost. Idempotent on the grapher side (EventSequence dedup).
    // Returns the number of chunks re-stashed.
    std::size_t requeueStalled(std::chrono::seconds max_age)
    {
        std::vector<StoryChunk*> to_stash;
        {
            std::lock_guard<std::mutex> lock(tailMutex);
            auto const now = std::chrono::steady_clock::now();
            for(auto& story_entry: storyRetention)
            {
                for(auto& chunk_entry: story_entry.second.chunks)
                {
                    StoryChunk* chunk = chunk_entry.first;
                    ChunkState& state = chunk_entry.second;
                    // covered-by-W only excuses a chunk that was acked: an
                    // unshipped chunk below W is the classic straggler (other
                    // keepers pushed W past its range while its own transfer
                    // kept failing) and must re-send — the grapher re-opens a
                    // past window for it (prepend path); without the re-send
                    // it would sit retained forever, unfreeable for lack of
                    // the ack.
                    if(state.in_queue ||
                       (state.shipped && chunk->getEndTime() <= story_entry.second.known_w))
                    {
                        continue;
                    }
                    if(now - state.last_activity < max_age)
                    {
                        continue;
                    }
                    state.in_queue = true;
                    state.last_activity = now;
                    to_stash.push_back(chunk);
                    LOG_INFO("[KeeperChunkRetentionStore] re-sending stalled StoryId={} chunk {}-{} (shipped={}, "
                             "known_W={})",
                             story_entry.first,
                             chunk->getStartTime(),
                             chunk->getEndTime(),
                             state.shipped,
                             story_entry.second.known_w);
                }
            }
        }
        for(auto* chunk: to_stash)
        {
            theExtractionQueue.stashStoryChunk(chunk);
        }
        return to_stash.size();
    }

    // Serve a replay range from every retained chunk (not only tail-indexed
    // events: a chunk evicted from the tail but still awaiting W holds events
    // that may exist nowhere else). Events are returned in ascending
    // EventSequence order, capped at max_events (truncated=true if the cap
    // cut the range short). hot_floor reports the oldest event tick this
    // keeper still retains for the story (UINT64_MAX if none): everything
    // below it has been freed, which the free condition only permits once it
    // is durable in the archive.
    std::vector<LogEvent> fetchRange(StoryId const& story_id, uint64_t start, uint64_t end, std::size_t max_events,
                                     uint64_t& hot_floor, bool& truncated)
    {
        std::vector<LogEvent> result;
        hot_floor = UINT64_MAX;
        truncated = false;
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(story_id);
        if(story_it == storyRetention.end())
        {
            return result;
        }
        std::map<EventSequence, LogEvent const*> merged;
        for(auto const& chunk_entry: story_it->second.chunks)
        {
            StoryChunk const* chunk = chunk_entry.first;
            if(!chunk->empty() && chunk->firstEventTime() < hot_floor)
            {
                hot_floor = chunk->firstEventTime();
            }
            for(auto it = chunk->lower_bound(start); it != chunk->end() && it->second.time() < end; ++it)
            {
                merged.emplace(it->first, &it->second);
            }
        }
        result.reserve(merged.size() < max_events ? merged.size() : max_events);
        for(auto const& entry: merged)
        {
            if(result.size() >= max_events)
            {
                truncated = true;
                break;
            }
            result.push_back(*entry.second);
        }
        return result;
    }

    // Number of chunks currently owned for the story (diagnostics/tests).
    std::size_t retainedChunkCount(StoryId const& story_id) const
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(story_id);
        return (story_it == storyRetention.end()) ? 0 : story_it->second.chunks.size();
    }

    // Phase 1: this keeper's most recent (up to n) EventSequences for the story.
    std::vector<EventSequence> getTailSequences(StoryId const& story_id, std::size_t n)
    {
        std::vector<EventSequence> result;
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(story_id);
        if(story_it == storyRetention.end())
        {
            return result;
        }
        auto& index = story_it->second.index;
        std::size_t take = (n < index.size()) ? n : index.size();
        result.reserve(take);
        // walk from the newest (largest) key backwards, collect `take` of them
        std::size_t skip = index.size() - take;
        auto it = index.begin();
        std::advance(it, skip);
        for(; it != index.end(); ++it)
        {
            result.push_back(it->first);
        }
        return result; // ascending order
    }

    // Phase 2: payloads for the requested EventSequences this keeper still holds.
    std::vector<LogEvent> getTailEvents(StoryId const& story_id, std::vector<EventSequence> const& seqs)
    {
        std::vector<LogEvent> result;
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(story_id);
        if(story_it == storyRetention.end())
        {
            return result;
        }
        auto& index = story_it->second.index;
        result.reserve(seqs.size());
        for(auto const& seq: seqs)
        {
            auto idx_it = index.find(seq);
            if(idx_it == index.end())
            {
                continue;
            }
            LogEvent const* event = idx_it->second->findEvent(seq);
            if(event != nullptr)
            {
                result.push_back(*event);
            }
        }
        return result;
    }

private:
    struct ChunkState
    {
        // number of this chunk's events still in the tail index
        std::size_t indexed_count = 0;
        // grapher acked a transfer of this chunk at least once
        bool shipped = false;
        // the chunk's pointer currently sits in the extraction queue (from the
        // seal-time stash or a stall re-send); freeing it now would leave a
        // dangling pointer for the drain thread
        bool in_queue = false;
        std::size_t approx_bytes = 0;
        std::chrono::steady_clock::time_point last_activity;
    };

    struct StoryRetention
    {
        // sorted last-N tail; the payload lives once inside the chunk pointed to.
        std::map<EventSequence, StoryChunk*> index;
        // owns the retained chunks
        std::unordered_map<StoryChunk*, ChunkState> chunks;
        // highest persisted watermark reported by the grapher for this story
        uint64_t known_w = 0;
    };

    static std::size_t approxChunkBytes(StoryChunk const* chunk)
    {
        std::size_t bytes = 0;
        for(auto it = chunk->begin(); it != chunk->end(); ++it)
        {
            bytes += it->second.logRecord.size() + 64; // payload + per-event bookkeeping estimate
        }
        return bytes;
    }

    // THE free condition, called from every mutating path (caller holds
    // tailMutex). Frees the chunk and erases its state when it is shipped,
    // covered by the known watermark, tail-released, and not queued. Returns
    // the iterator following the (possibly erased) entry.
    std::unordered_map<StoryChunk*, ChunkState>::iterator
    maybeFreeChunk(StoryRetention& story, std::unordered_map<StoryChunk*, ChunkState>::iterator chunk_iter)
    {
        StoryChunk* chunk = chunk_iter->first;
        ChunkState const& state = chunk_iter->second;
        if(!state.shipped || state.in_queue || state.indexed_count != 0 || chunk->getEndTime() > story.known_w)
        {
            return ++chunk_iter;
        }
        LOG_INFO("[KeeperChunkRetentionStore] freeing StoryId={} chunk {}-{} eventCount {} (W={} covers it)",
                 chunk->getStoryId(),
                 chunk->getStartTime(),
                 chunk->getEndTime(),
                 chunk->getEventCount(),
                 story.known_w);
        retainedBytes -= (state.approx_bytes < retainedBytes) ? state.approx_bytes : retainedBytes;
        auto next = story.chunks.erase(chunk_iter);
        delete chunk;
        return next;
    }

    // Evict oldest events until the tail is within capacity. Eviction only
    // trims the read index; the chunk itself is freed solely by the free
    // condition above.
    void enforceCapacity(StoryRetention& story)
    {
        while(story.index.size() > tailCapacity)
        {
            auto oldest = story.index.begin();
            StoryChunk* chunk = oldest->second;
            story.index.erase(oldest);
            auto chunk_it = story.chunks.find(chunk);
            if(chunk_it != story.chunks.end() && chunk_it->second.indexed_count > 0)
            {
                if(--chunk_it->second.indexed_count == 0)
                {
                    maybeFreeChunk(story, chunk_it);
                }
            }
        }
    }

    // WARN once per crossing of the retention cap; never drops data.
    void warnOnCapCrossing()
    {
        if(retentionCapBytes == 0)
        {
            return;
        }
        if(retainedBytes > retentionCapBytes && !capWarned)
        {
            capWarned = true;
            LOG_WARNING("[KeeperChunkRetentionStore] retained ~{} MB exceeds retention_cap_mb={} — the grapher's "
                        "persisted watermark is lagging (outage?); retaining anyway, data is never dropped",
                        retainedBytes / (1024 * 1024),
                        retentionCapBytes / (1024 * 1024));
        }
        else if(retainedBytes <= retentionCapBytes)
        {
            capWarned = false;
        }
    }

    StoryChunkExtractionQueue& theExtractionQueue;
    std::size_t tailCapacity;
    std::size_t retentionCapBytes;
    std::size_t retainedBytes = 0;
    bool capWarned = false;
    mutable std::mutex tailMutex;
    std::unordered_map<StoryId, StoryRetention> storyRetention;
};

} // namespace chronolog

#endif
