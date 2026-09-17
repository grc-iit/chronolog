#ifndef CHRONOLOG_STORY_WATERMARK_REGISTRY_H
#define CHRONOLOG_STORY_WATERMARK_REGISTRY_H

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <set>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <ChunkReceipt.h>
#include <ReceiptTracker.h>

namespace chronolog
{

// Per-story persisted watermark W: the end of the longest contiguous run of
// persisted merged timeline windows anchored at the story start. Everything
// below W for the story is written+flushed to HDF5.
//
// The contiguous-prefix rule (not max(end)) is required because the grapher's
// extraction module drains on multiple streams, so merged windows of one story
// can persist out of order, and a failed HDF5 write must hold W back — keepers
// then retain and re-send rather than freeing unpersisted data.
//
// W lives in memory only: a grapher restart resets it, keepers re-send
// everything still retained, and read-side EventSequence dedup cleans the
// resulting duplicates.
//
// W covering a keeper's chunk does not prove the chunk was written: a chunk
// merged after its range persisted lands in a reopened window or a salvage
// file, neither of which moves W. So every arriving chunk gets a receipt,
// numbered per story, which settles once the chunk is merged and every chunk
// holding its events is written. Reports carry the unsettled receipts next to
// W (see ChunkReceipt.h).
class StoryWatermarkRegistry: public ReceiptTracker
{
public:
    // Anchor for contiguity. Called from GrapherDataStore::startStoryRecording.
    // If the story is already known with W >= start_time, keeps W (re-acquired
    // story). If start_time > current W, the gap [W, start_time) is treated as
    // covered (no events were recorded in it by this grapher) — but ONLY when
    // that claim is provable: the registration created a fresh pipeline (a
    // live pipeline's open windows may hold received-but-unpersisted events),
    // no HDF5 write for the story has ever failed (the failed window's events
    // are not on disk; the keepers' stall re-send is the recovery, and it needs
    // W held back), and nothing is parked above a persistence gap. Otherwise W
    // stands and catches up through advancePersisted alone.
    void registerStory(StoryId const& story_id, uint64_t start_time, bool fresh_pipeline = false)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto iter = stories.find(story_id);
        if(iter != stories.end() && dropped.count(story_id) != 0)
        {
            // The id was destroyed and its drop has not been reported yet: this
            // is a different incarnation that happens to hash to the same id.
            // Start it over rather than letting it inherit the drop watermark,
            // which would tell every keeper to free the new story's chunks.
            // Its keepers keep the old incarnation's chunks (see dropStory).
            LOG_INFO("[StoryWatermarkRegistry] StoryId={} registered again before its drop was reported; "
                     "starting the new incarnation at {}",
                     story_id,
                     start_time);
            stories.erase(iter);
            receipts.erase(story_id);
            dropped.erase(story_id);
            dirty.erase(story_id);
            iter = stories.end();
        }
        if(iter == stories.end())
        {
            Entry entry;
            entry.anchor = start_time;
            entry.w = start_time;
            // a window of this story failed to write before it was registered
            entry.write_failed = failedBeforeRegistration.erase(story_id) != 0;
            stories.emplace(story_id, std::move(entry));
            dirty.insert(story_id);
            LOG_INFO("[StoryWatermarkRegistry] StoryId={} registered, anchor={}{}",
                     story_id,
                     start_time,
                     stories[story_id].write_failed ? " (carrying an earlier write failure)" : "");
            return;
        }
        Entry& entry = iter->second;
        if(start_time > entry.w)
        {
            if(!fresh_pipeline || entry.write_failed || !entry.pending.empty())
            {
                LOG_INFO("[StoryWatermarkRegistry] StoryId={} re-registered at {}, W held at {} "
                         "(fresh_pipeline={}, write_failed={}, parked_intervals={})",
                         story_id,
                         start_time,
                         entry.w,
                         fresh_pipeline,
                         entry.write_failed,
                         entry.pending.size());
                return;
            }
            LOG_INFO("[StoryWatermarkRegistry] StoryId={} re-registered at {}, covering idle gap from W={}",
                     story_id,
                     start_time,
                     entry.w);
            entry.w = start_time;
            dirty.insert(story_id);
        }
        // else: re-acquired story, W stands
    }

    // The story was destroyed: its archive files are deleted and every chunk
    // that arrives for it from now on is refused (see
    // GrapherDataStore::startStoryRecording), so nothing this grapher does can
    // ever settle the receipts its keepers are holding. Report the story once
    // more with kStoryDroppedWatermark, which tells them to free what they hold,
    // then forget it so a later acquisition of the same name starts fresh.
    //
    // KNOWN GAP — recreate under the same name. Story ids are a deterministic
    // CityHash64(chronicle + story), so destroying a story and creating it again
    // under the same name yields the same id. Two orderings are lossy:
    //  - the new registration reaches this grapher before the drop report is
    //    published: registerStory re-creates the entry, the drop is never
    //    reported, and the old chunks stay retained on the keepers exactly as
    //    they do today (no corruption, the leak this method fixes just persists
    //    for that story);
    //  - the drop report arrives at a keeper after it has already sealed chunks
    //    for the NEW story: the keeper frees those too, since it cannot tell the
    //    two incarnations apart, and their events are then only wherever the
    //    grapher put them.
    // Both need the report to carry the incarnation it belongs to (a story
    // epoch) to close properly; see the destroyed-story retention follow-up.
    void dropStory(StoryId const& story_id)
    {
        std::lock_guard<std::mutex> lock(mtx);
        dropped.insert(story_id);
        dirty.insert(story_id);
        auto iter = stories.find(story_id);
        if(iter == stories.end())
        {
            // Never recorded here, and still worth reporting: a chunk that
            // arrives after the destroy is refused, so the story is never
            // registered, yet the keeper that sent it holds that chunk and has
            // nothing else to wait for. It is a known contributor, since its
            // chunk did arrive, so the report reaches it.
            LOG_INFO("[StoryWatermarkRegistry] StoryId={} dropped; reporting the drop watermark for a story this "
                     "grapher never recorded",
                     story_id);
            return;
        }
        iter->second.w = kStoryDroppedWatermark;
        auto receipts_iter = receipts.find(story_id);
        if(receipts_iter != receipts.end())
        {
            // no chunk of a destroyed story is ever written, so nothing stays
            // outstanding; the report carries the story's highest receipt with
            // an empty pending list
            receipts_iter->second.pending.clear();
        }
        dropped.insert(story_id);
        dirty.insert(story_id);
        LOG_INFO("[StoryWatermarkRegistry] StoryId={} dropped; reporting the drop watermark once", story_id);
    }

    // A merged window's HDF5 write failed: its events were received but are
    // not durable. Sticky — from here on the story's W may advance only
    // through actual persisted intervals (advancePersisted), never by
    // re-registration gap coverage.
    void persistFailed(StoryId const& story_id)
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto iter = stories.find(story_id);
        if(iter == stories.end())
        {
            // Held aside rather than stored as an entry. An entry here would
            // anchor the story at 0, and because the failure also bars
            // registerStory from covering the gap up to the story's start, W
            // would stay at 0 for the life of this grapher -- no report would
            // ever let the story's keepers free anything. Applied to the entry
            // when the story does register.
            failedBeforeRegistration.insert(story_id);
            LOG_WARNING("[StoryWatermarkRegistry] StoryId={} write failure recorded for unregistered story", story_id);
            return;
        }
        iter->second.write_failed = true;
        LOG_WARNING("[StoryWatermarkRegistry] StoryId={} write failure recorded, W held at {}",
                    story_id,
                    iter->second.w);
    }

    // Persisted merged window [start, end). W advances to the end of the
    // longest contiguous run of persisted intervals from the anchor. Intervals
    // at or below W are ignored (re-persisted straggler windows — W never
    // regresses). Out-of-order safe.
    void advancePersisted(StoryId const& story_id, uint64_t start, uint64_t end)
    {
        if(end <= start)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(mtx);
        auto iter = stories.find(story_id);
        if(iter == stories.end())
        {
            // Story never registered (adoption/recovery path): the first
            // persisted interval anchors it.
            Entry entry;
            entry.anchor = start;
            entry.w = start;
            iter = stories.emplace(story_id, std::move(entry)).first;
        }
        Entry& entry = iter->second;
        if(end <= entry.w)
        {
            LOG_DEBUG("[StoryWatermarkRegistry] StoryId={} interval {}-{} at or below W={}, ignored",
                      story_id,
                      start,
                      end,
                      entry.w);
            return;
        }
        auto emplace_result = entry.pending.emplace(start, end);
        if(!emplace_result.second && end > emplace_result.first->second)
        {
            emplace_result.first->second = end;
        }
        uint64_t const previous_w = entry.w;
        absorbContiguous(entry);
        if(entry.w != previous_w)
        {
            dirty.insert(story_id);
            LOG_INFO("[StoryWatermarkRegistry] StoryId={} W advanced {} -> {}", story_id, previous_w, entry.w);
        }
        else
        {
            LOG_DEBUG("[StoryWatermarkRegistry] StoryId={} interval {}-{} parked, W held at {} (gap below)",
                      story_id,
                      start,
                      end,
                      entry.w);
        }
    }

    // Current W for the story; 0 if unknown.
    uint64_t getPersisted(StoryId const& story_id) const
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto iter = stories.find(story_id);
        return (iter == stories.end()) ? 0 : iter->second.w;
    }

    // Id of this grapher process: nonzero and drawn at random, so a keeper can
    // tell a restarted grapher's receipts from the previous instance's.
    uint64_t instanceId() const { return instance; }

    // A chunk for the story arrived. Receipts are numbered from 1 per story and
    // stay pending until the chunk is merged and every chunk holding its events
    // is written. chunk_end is the end of the arriving chunk, which decides
    // whether a report has to carry the receipt while it is pending (see
    // snapshotDirty); 0 means unknown, and is always carried.
    uint64_t assignReceipt(StoryId const& story_id, uint64_t chunk_end = 0)
    {
        std::lock_guard<std::mutex> lock(mtx);
        StoryReceipts& story = receipts[story_id];
        uint64_t const receipt = ++story.last_assigned;
        ReceiptState state;
        state.chunk_end = chunk_end;
        story.pending.emplace(receipt, state);
        return receipt;
    }

    // A timeline window or salvage chunk now holds events of the receipt.
    void holdReceipt(StoryId const& story_id, uint64_t receipt) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        ReceiptState* state = findPending(story_id, receipt);
        if(state != nullptr)
        {
            state->holders++;
        }
    }

    // One chunk holding events of the receipt was written.
    void releaseReceipt(StoryId const& story_id, uint64_t receipt) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        ReceiptState* state = findPending(story_id, receipt);
        if(state != nullptr && state->holders > 0)
        {
            state->holders--;
            settleIfWritten(story_id, receipt, *state);
        }
    }

    // The receipt's chunk is merged and none of its events was discarded.
    void receiptMerged(StoryId const& story_id, uint64_t receipt) override
    {
        std::lock_guard<std::mutex> lock(mtx);
        ReceiptState* state = findPending(story_id, receipt);
        if(state != nullptr)
        {
            state->merged = true;
            settleIfWritten(story_id, receipt, *state);
        }
    }

    // Reports for the stories that changed since the last snapshot; clears the
    // dirty set.
    std::map<StoryId, StoryWatermarkReport> snapshotDirty()
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::map<StoryId, StoryWatermarkReport> snapshot;
        for(auto const& story_id: dirty)
        {
            auto iter = stories.find(story_id);
            if(iter == stories.end() && dropped.count(story_id) != 0)
            {
                // a story destroyed before this grapher ever recorded it; its
                // keepers still hold the chunks they sent afterwards
                StoryWatermarkReport report;
                report.watermark = kStoryDroppedWatermark;
                report.grapher_instance = instance;
                snapshot.emplace(story_id, std::move(report));
                continue;
            }
            if(iter != stories.end())
            {
                StoryWatermarkReport report;
                report.watermark = iter->second.w;
                report.grapher_instance = instance;
                auto receipts_iter = receipts.find(story_id);
                if(receipts_iter != receipts.end())
                {
                    report.highest_receipt = receipts_iter->second.last_assigned;
                    for(auto const& pending: receipts_iter->second.pending)
                    {
                        // A receipt whose chunk ends above W needs no mention: a
                        // keeper frees a chunk only when W covers it as well, so
                        // W already holds that chunk back. Leaving those out
                        // bounds the report when a receipt can never settle — a
                        // write that failed, or events a merge had to discard —
                        // instead of carrying it in every later report.
                        if(pending.second.chunk_end == 0 || pending.second.chunk_end <= iter->second.w)
                        {
                            report.pending_receipts.push_back(pending.first);
                        }
                    }
                }
                snapshot.emplace(story_id, std::move(report));
            }
        }
        dirty.clear();
        // A dropped story is reported once: forget it now that its report is on
        // its way, so the id is free for a later story of the same name.
        for(auto const& story_id: dropped)
        {
            stories.erase(story_id);
            receipts.erase(story_id);
        }
        dropped.clear();
        return snapshot;
    }

private:
    struct Entry
    {
        uint64_t anchor = 0;
        uint64_t w = 0;
        // an HDF5 write for this story failed at least once (sticky)
        bool write_failed = false;
        // start -> end of persisted-but-not-yet-contiguous intervals
        std::map<uint64_t, uint64_t> pending;
    };

    // Fold every pending interval that touches the prefix into W. pending is
    // ordered by start, so one forward pass suffices: absorbing an interval can
    // only raise W, and once an interval's start exceeds W every later one
    // does too.
    static void absorbContiguous(Entry& entry)
    {
        for(auto p = entry.pending.begin(); p != entry.pending.end();)
        {
            if(p->first > entry.w)
            {
                break;
            }
            if(p->second > entry.w)
            {
                entry.w = p->second;
            }
            p = entry.pending.erase(p);
        }
    }

    struct ReceiptState
    {
        // chunks holding events of the receipt that are not written yet
        uint32_t holders = 0;
        // every event of the receipt's chunk went into a holding chunk
        bool merged = false;
        // end of the chunk this receipt was given to; 0 when unknown
        uint64_t chunk_end = 0;
    };

    // Kept apart from Entry: a chunk can arrive before its story registers,
    // and an Entry created then would anchor W at 0.
    struct StoryReceipts
    {
        uint64_t last_assigned = 0;
        std::map<uint64_t, ReceiptState> pending;
    };

    ReceiptState* findPending(StoryId const& story_id, uint64_t receipt)
    {
        auto story_iter = receipts.find(story_id);
        if(story_iter == receipts.end())
        {
            return nullptr;
        }
        auto receipt_iter = story_iter->second.pending.find(receipt);
        return (receipt_iter == story_iter->second.pending.end()) ? nullptr : &receipt_iter->second;
    }

    void settleIfWritten(StoryId const& story_id, uint64_t receipt, ReceiptState const& state)
    {
        if(!state.merged || state.holders != 0)
        {
            return;
        }
        receipts[story_id].pending.erase(receipt);
        dirty.insert(story_id);
    }

    static uint64_t drawInstanceId()
    {
        std::random_device device;
        uint64_t const id = ((static_cast<uint64_t>(device()) << 32) | device()) ^
                            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return (id == 0) ? 1 : id;
    }

    uint64_t const instance = drawInstanceId();
    mutable std::mutex mtx;
    std::map<StoryId, Entry> stories;
    std::map<StoryId, StoryReceipts> receipts;
    std::set<StoryId> dirty;
    // destroyed stories whose drop report has not gone out yet
    std::set<StoryId> dropped;
    // stories whose window failed to write before they were registered here;
    // folded into the entry at registration (see persistFailed)
    std::set<StoryId> failedBeforeRegistration;
};

} // namespace chronolog

#endif // CHRONOLOG_STORY_WATERMARK_REGISTRY_H
