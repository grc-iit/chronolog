#ifndef KEEPER_CHUNK_RETENTION_STORE_H
#define KEEPER_CHUNK_RETENTION_STORE_H

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <unordered_map>

#include <chronolog_types.h>
#include <ChunkReceipt.h>
#include <HotRangeResponse.h>
#include <StoryChunk.h>

#include "ActiveTailSource.h"
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
//   AND the ack's receipt is settled (the grapher reported the chunk's events
//   written; a chunk shipped without a receipt counts as settled) AND tail
//   released (no indexed events left) AND not sitting in the extraction queue
//   (a re-send in flight must never dangle).
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
    KeeperChunkRetentionStore(StoryChunkExtractionQueue& extraction_queue,
                              std::size_t tail_capacity,
                              std::size_t retention_cap_mb = 0,
                              bool live_tail_read = false,
                              std::chrono::milliseconds archive_visibility_delay = std::chrono::milliseconds(0))
        : theExtractionQueue(extraction_queue)
        , tailCapacity(tail_capacity)
        , retentionCapBytes(retention_cap_mb * 1024 * 1024)
        , liveTailRead(live_tail_read)
        , archiveVisibilityDelay(archive_visibility_delay)
    {}

    // Hand every not-yet-shipped chunk to the extraction queue WHILE extraction is
    // still running. waitUntilDurable() calls this on every round of the keeper's
    // shutdown wait, after data collection stops and before shutdownExtraction().
    //
    // The destructor already has a last-chance handoff for these, but it runs too
    // late to help: by then shutdownExtraction() has drained the queue and joined
    // the extraction xstreams, and StoryChunkExtractionQueue::shutDown() simply
    // deletes whatever is still queued. Everything sealed but not yet shipped was
    // therefore freed rather than archived, on every clean shutdown.
    //
    // Marking in_queue is the same contract requeueStalled() uses when it re-stashes:
    // the queue owns the pointer from here on, the drain callbacks clear the flag,
    // and the destructor's `if(state.in_queue) continue` skips what we handed over,
    // so nothing is double-freed. Idempotent: a second call finds nothing unshipped.
    std::size_t flushUnshippedChunks()
    {
        std::vector<StoryChunk*> to_stash;
        {
            std::lock_guard<std::mutex> lock(tailMutex);
            for(auto& story_entry: storyRetention)
            {
                for(auto& chunk_entry: story_entry.second.chunks)
                {
                    ChunkState& state = chunk_entry.second;
                    if(state.in_queue || state.shipped)
                    {
                        continue;
                    }
                    to_stash.push_back(chunk_entry.first);
                    state.in_queue = true;
                }
            }
        }
        // stash outside the lock, as requeueStalled does
        for(auto* chunk: to_stash) { theExtractionQueue.stashStoryChunk(chunk); }
        if(!to_stash.empty())
        {
            LOG_INFO("[KeeperChunkRetentionStore] Flushed {} unshipped chunk(s) to the extraction queue",
                     to_stash.size());
        }
        return to_stash.size();
    }

    // Keeper shutdown, after data collection has stopped and while extraction
    // and watermark reports still run. Hands every chunk the grapher has not
    // acked to the extraction queue again, every poll_interval, until the
    // grapher has confirmed every chunk written or timeout passes. A send that
    // fails after one round is picked up by the next. Returns whether every
    // chunk was confirmed; the destructor logs and frees the rest.
    bool waitUntilDurable(std::chrono::milliseconds timeout, std::chrono::milliseconds poll_interval)
    {
        auto const deadline = std::chrono::steady_clock::now() + timeout;
        while(true)
        {
            flushUnshippedChunks();
            // and send again what was acked but never confirmed: the grapher's
            // write may have failed, and its receipt then never settles. The
            // stall timer that normally does this runs on the data-collection
            // loop, which has already stopped.
            requeueStalled(std::chrono::duration_cast<std::chrono::seconds>(poll_interval));
            if(unconfirmedChunkCount() == 0)
            {
                return true;
            }
            if(std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(poll_interval);
        }
    }

    ~KeeperChunkRetentionStore()
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        for(auto& story_entry: storyRetention)
        {
            for(auto& chunk_entry: story_entry.second.chunks)
            {
                StoryChunk* chunk = chunk_entry.first;
                ChunkState& state = chunk_entry.second;
                bool const durable = confirmedWritten(story_entry.second, chunk, state);
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
        // the receipt of this delivery; a re-send replaces the earlier one
        chunk_it->second.receipt_instance = chunk->getGrapherInstance();
        chunk_it->second.receipt = chunk->getGrapherReceipt();
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

    // A watermark without receipt information: everything below w for this
    // story is durable. Used where durability is local (an extraction chain
    // with no grapher) and by tests; a chunk shipped under a grapher receipt
    // still waits for applyReport to settle it.
    void confirmPersisted(StoryId const& story_id, uint64_t w)
    {
        StoryWatermarkReport report;
        report.watermark = w;
        applyReport(story_id, report);
    }

    // The grapher's report for one story: everything below the watermark is
    // written, and so is every receipt of grapher_instance up to
    // highest_receipt except the pending ones. Keeps the higher of the known
    // and the reported watermark, folds the receipts into the story's receipt
    // view (see mergeReceipts), then frees every chunk meeting the free
    // condition. A lower watermark comes from a report that arrived late, or
    // from a restarted grapher that counts W from a new start; its receipts
    // still count, or a chunk re-sent to a restarted grapher could not settle
    // until the new W passed the old one.
    void applyReport(StoryId const& story_id, StoryWatermarkReport const& report)
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        StoryRetention& story = storyRetention[story_id];
        if(report.watermark < story.known_w)
        {
            LOG_DEBUG("[KeeperChunkRetentionStore] StoryId={} reported watermark {} below known {}; keeping the known",
                      story_id,
                      report.watermark,
                      story.known_w);
        }
        else
        {
            story.known_w = report.watermark;
        }
        if(report.grapher_instance != 0)
        {
            mergeReceipts(story, report);
        }
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
                    // covered-by-W only excuses a chunk acked under a settled receipt: an
                    // unshipped chunk below W is the classic straggler (other
                    // keepers pushed W past its range while its own transfer
                    // kept failing) and must re-send — the grapher re-opens a
                    // past window for it (prepend path); without the re-send
                    // it would sit retained forever, unfreeable for lack of
                    // the ack.
                    if(state.in_queue || confirmedWritten(story_entry.second, chunk, state))
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
        for(auto* chunk: to_stash) { theExtractionQueue.stashStoryChunk(chunk); }
        return to_stash.size();
    }

    // Free every chunk the free condition allows now. A chunk that is durable
    // but was written less than the archive visibility delay ago survives the
    // report that made it durable, and no later report has to come for it;
    // the data-collection loop calls this to free it once the delay has
    // passed. Returns the number of chunks freed.
    std::size_t freeDurableChunks()
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        std::size_t freed = 0;
        for(auto& story_entry: storyRetention)
        {
            auto& chunks = story_entry.second.chunks;
            for(auto chunk_iter = chunks.begin(); chunk_iter != chunks.end();)
            {
                std::size_t const before = chunks.size();
                chunk_iter = maybeFreeChunk(story_entry.second, chunk_iter);
                freed += before - chunks.size();
            }
        }
        return freed;
    }

    // Serve a replay range from every retained chunk (not only tail-indexed
    // events: a chunk evicted from the tail but still awaiting W holds events
    // that may exist nowhere else). Events from chunks visible in the archive
    // (acknowledged under a settled receipt at least the archive visibility
    // delay ago) go to events, the rest to unconfirmed_events: a player may
    // not find those in the archive however far W moves. Both lists are in ascending
    // EventSequence order and together capped at max_events (truncated if the
    // cap cut the range short). hot_floor is the oldest event tick this keeper
    // still retains for the story (UINT64_MAX if none) and known_W the
    // watermark it last saw, read under the same lock as the events;
    // HotRangeSplit.h explains how the player splits a replay with them.
    HotRangeResponse fetchRange(StoryId const& story_id, uint64_t start, uint64_t end, std::size_t max_events)
    {
        HotRangeResponse response;
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(story_id);
        if(story_it == storyRetention.end())
        {
            return response;
        }
        response.known_W = story_it->second.known_w;
        auto const now = std::chrono::steady_clock::now();
        // event -> (payload, held by a chunk visible in the archive)
        std::map<EventSequence, std::pair<LogEvent const*, bool>> merged;
        for(auto& chunk_entry: story_it->second.chunks)
        {
            StoryChunk const* chunk = chunk_entry.first;
            bool const acknowledged = visibleInArchive(story_it->second, chunk_entry.second, now);
            if(!chunk->empty() && chunk->firstEventTime() < response.hot_floor)
            {
                response.hot_floor = chunk->firstEventTime();
            }
            for(auto it = chunk->lower_bound(start); it != chunk->end() && it->second.time() < end; ++it)
            {
                auto emplaced = merged.emplace(it->first, std::make_pair(&it->second, acknowledged));
                // a copy in any acknowledged chunk is enough for the archive to have it
                emplaced.first->second.second = emplaced.first->second.second || acknowledged;
            }
        }
        std::size_t served = 0;
        for(auto const& entry: merged)
        {
            if(served >= max_events)
            {
                response.truncated = true;
                break;
            }
            if(entry.second.second)
            {
                response.events.push_back(*entry.second.first);
            }
            else
            {
                response.unconfirmed_events.push_back(*entry.second.first);
            }
            ++served;
        }
        return response;
    }

    // Number of chunks currently owned for the story (diagnostics/tests).
    std::size_t retainedChunkCount(StoryId const& story_id) const
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(story_id);
        return (story_it == storyRetention.end()) ? 0 : story_it->second.chunks.size();
    }

    // The story is no longer recorded on this keeper, so its tail stops serving
    // playback. Without this, tail_capacity eviction is the only way events
    // leave the index, and a story that retires with fewer events than that
    // keeps its chunks until the keeper restarts. Drops the story's index and
    // frees every chunk that is already durable; the others free when the
    // watermark reaches them. The story's known watermark stays: replay relies
    // on it to know what this keeper may have freed.
    void releaseStoryTail(StoryId const& story_id)
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        auto story_it = storyRetention.find(story_id);
        if(story_it == storyRetention.end())
        {
            return;
        }
        StoryRetention& story = story_it->second;
        story.index.clear();
        for(auto chunk_iter = story.chunks.begin(); chunk_iter != story.chunks.end();)
        {
            chunk_iter->second.indexed_count = 0;
            chunk_iter = maybeFreeChunk(story, chunk_iter);
        }
    }

    // A live story's pipeline registers itself here so tail reads can also serve the
    // active (unsealed) window when live_tail_read is enabled. The pipeline
    // unregisters in its destructor, before finalize(), so a query can never reach a
    // freed pipeline (see the activeSourcesMutex hold in the query paths).
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
    //
    // With live_tail_read enabled the sealed-tail last-N is unioned with the active
    // timeline's last-N. Deliberately holds NO lock itself: the sealed and active
    // halves each take their own mutex and release it before the other is consulted,
    // so tailMutex and the pipeline's sequencingMutex are never held together. That
    // preserves the seal path's pipeline->store lock order and keeps this read path
    // from inverting it.
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

    // The sealed half of the tail: everything this store actually retains.
    std::vector<EventSequence> getSealedTailSequences(StoryId const& story_id, std::size_t n)
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
        // Position `take` entries back from end() -- O(take) -- rather than
        // advancing O(index.size() - take) forward from begin() (which costs
        // ~tailCapacity node-steps to fetch the last few events of a full tail),
        // then walk forward to end() so the result comes out ascending.
        auto it = index.end();
        for(std::size_t i = 0; i < take; ++i) { --it; }
        for(; it != index.end(); ++it) { result.push_back(it->first); }
        return result; // ascending order
    }

    // Phase 2: payloads for the requested EventSequences this keeper still holds.
    //
    // A sequence that is not in the retained index is, with live_tail_read enabled,
    // looked up in the active timeline (an event whose chunk has not sealed yet).
    // Note the story-not-retained case does not return early: a story whose chunks
    // have all been freed -- or which has not sealed one yet -- can still have
    // servable active events.
    std::vector<LogEvent> getTailEvents(StoryId const& story_id, std::vector<EventSequence> const& seqs)
    {
        std::vector<LogEvent> result;
        result.reserve(seqs.size());
        std::vector<EventSequence> misses;

        {
            std::lock_guard<std::mutex> lock(tailMutex);
            auto story_it = storyRetention.find(story_id);
            auto* index = (story_it == storyRetention.end()) ? nullptr : &story_it->second.index;
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
        // (preserves the pipeline->store lock order used by the seal/decay path).
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
        // receipt the grapher returned for the last successful delivery (0: none)
        uint64_t receipt_instance = 0;
        uint64_t receipt = 0;
        // when the keeper first saw the chunk shipped under a settled receipt
        bool written = false;
        std::chrono::steady_clock::time_point written_at;
    };

    struct StoryRetention
    {
        // sorted last-N tail; the payload lives once inside the chunk pointed to.
        std::map<EventSequence, StoryChunk*> index;
        // owns the retained chunks
        std::unordered_map<StoryChunk*, ChunkState> chunks;
        // highest persisted watermark reported by the grapher for this story
        uint64_t known_w = 0;
        // the latest report's receipt view: receipts of report_instance up to
        // highest_receipt are written, except pending_receipts
        uint64_t report_instance = 0;
        uint64_t highest_receipt = 0;
        std::set<uint64_t> pending_receipts;
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

    // The grapher has acked the chunk, settled its receipt, and W covers it.
    static bool confirmedWritten(StoryRetention const& story, StoryChunk const* chunk, ChunkState const& state)
    {
        return state.shipped && receiptSettled(story, state) && chunk->getEndTime() <= story.known_w;
    }

    std::size_t unconfirmedChunkCount() const
    {
        std::lock_guard<std::mutex> lock(tailMutex);
        std::size_t count = 0;
        for(auto const& story_entry: storyRetention)
        {
            for(auto const& chunk_entry: story_entry.second.chunks)
            {
                if(!confirmedWritten(story_entry.second, chunk_entry.first, chunk_entry.second))
                {
                    ++count;
                }
            }
        }
        return count;
    }

    // A grapher never moves a receipt back to pending or reuses its number, so
    // reports from one grapher instance combine in any order: a receipt is
    // written once any report says so. A report from another instance replaces
    // the view, since that instance numbers its receipts afresh.
    static void mergeReceipts(StoryRetention& story, StoryWatermarkReport const& report)
    {
        std::set<uint64_t> const reported_pending(report.pending_receipts.begin(), report.pending_receipts.end());
        if(report.grapher_instance != story.report_instance)
        {
            story.report_instance = report.grapher_instance;
            story.highest_receipt = report.highest_receipt;
            story.pending_receipts = reported_pending;
            return;
        }
        std::set<uint64_t> pending;
        for(uint64_t const receipt: story.pending_receipts)
        {
            // pending in the view, unless the report covers it and does not list it
            if(receipt > report.highest_receipt || reported_pending.count(receipt) != 0)
            {
                pending.insert(receipt);
            }
        }
        for(uint64_t const receipt: reported_pending)
        {
            // pending in the report, unless the view already covers it as written
            if(receipt > story.highest_receipt)
            {
                pending.insert(receipt);
            }
        }
        story.highest_receipt = std::max(story.highest_receipt, report.highest_receipt);
        story.pending_receipts = std::move(pending);
    }

    // A chunk shipped without a receipt (no grapher on the path) is settled by
    // its ack. Otherwise the story's receipt view has to come from the same
    // grapher instance, reach the receipt, and not list it as pending.
    static bool receiptSettled(StoryRetention const& story, ChunkState const& state)
    {
        return state.receipt == 0 ||
               (state.receipt_instance == story.report_instance && state.receipt <= story.highest_receipt &&
                story.pending_receipts.count(state.receipt) == 0);
    }

    // Whether a player can be expected to find the chunk in the archive: it is
    // shipped under a settled receipt, and the keeper first saw it so at least
    // archiveVisibilityDelay ago. Records that first sighting (caller holds
    // tailMutex).
    bool
    visibleInArchive(StoryRetention const& story, ChunkState& state, std::chrono::steady_clock::time_point now) const
    {
        if(!state.shipped || !receiptSettled(story, state))
        {
            return false;
        }
        if(!state.written)
        {
            state.written = true;
            state.written_at = now;
        }
        return now - state.written_at >= archiveVisibilityDelay;
    }

    // THE free condition, called from every mutating path and from
    // freeDurableChunks (caller holds tailMutex). Frees the chunk and erases
    // its state when it is visible in the archive, covered by the known
    // watermark, tail-released, and not queued. Returns the iterator following
    // the (possibly erased) entry.
    std::unordered_map<StoryChunk*, ChunkState>::iterator
    maybeFreeChunk(StoryRetention& story, std::unordered_map<StoryChunk*, ChunkState>::iterator chunk_iter)
    {
        StoryChunk* chunk = chunk_iter->first;
        ChunkState& state = chunk_iter->second;
        // checked first, so the sighting is recorded whatever else keeps the chunk
        bool const visible = visibleInArchive(story, state, std::chrono::steady_clock::now());
        if(!visible || state.in_queue || state.indexed_count != 0 || chunk->getEndTime() > story.known_w)
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

    // The active (unsealed) half of the tail, served by the story's live pipeline.
    // Holds activeSourcesMutex across the call so the pipeline cannot be unregistered
    // and freed while its timeline is being walked. Takes no other lock, so the
    // pipeline's own mutex is only ever acquired with tailMutex released.
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

    StoryChunkExtractionQueue& theExtractionQueue;
    std::size_t tailCapacity;
    std::size_t retentionCapBytes;
    std::size_t retainedBytes = 0;
    bool capWarned = false;
    bool liveTailRead = false;
    // how long after the keeper learns a chunk is written a player may still
    // not see its archive file
    std::chrono::milliseconds archiveVisibilityDelay;
    mutable std::mutex tailMutex;
    std::unordered_map<StoryId, StoryRetention> storyRetention;

    // Guarded independently of tailMutex; see the lock-order note on getTailSequences.
    std::mutex activeSourcesMutex;
    std::unordered_map<StoryId, ActiveTailSource*> activeSources;
};

} // namespace chronolog

#endif
