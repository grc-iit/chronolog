#ifndef CHRONOLOG_STORY_WATERMARK_REGISTRY_H
#define CHRONOLOG_STORY_WATERMARK_REGISTRY_H

#include <cstdint>
#include <map>
#include <mutex>
#include <set>

#include <chrono_monitor.h>
#include <chronolog_types.h>

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
class StoryWatermarkRegistry
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
        if(iter == stories.end())
        {
            Entry entry;
            entry.anchor = start_time;
            entry.w = start_time;
            stories.emplace(story_id, std::move(entry));
            dirty.insert(story_id);
            LOG_INFO("[StoryWatermarkRegistry] StoryId={} registered, anchor={}", story_id, start_time);
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
            Entry entry;
            entry.write_failed = true;
            stories.emplace(story_id, std::move(entry));
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
    // Seed the registry from a manifest's persisted intervals after a restart.
    //
    // Replays them through advancePersisted rather than assigning W directly, so
    // the anchoring, prefix-absorption and parking rules are the ones already used
    // at runtime -- and, crucially, so intervals sitting above a persistence gap
    // stay parked. Assigning only the final W would forget them, and the window
    // that later closes the gap would advance W to the gap's end rather than past
    // everything already durable, forcing keepers to re-send chunks that are
    // safely on disk. Restored stories come back dirty so the next publish tells
    // the keepers what they may release.
    void restoreFromManifest(std::map<StoryId, std::map<uint64_t, uint64_t>> const& intervals)
    {
        std::size_t restored_stories = 0;
        for(auto const& story_entry: intervals)
        {
            for(auto const& interval: story_entry.second)
            {
                advancePersisted(story_entry.first, interval.first, interval.second);
            }
            restored_stories++;
        }
        LOG_INFO("[StoryWatermarkRegistry] Restored {} story watermark(s) from the archive manifest", restored_stories);
    }

    uint64_t getPersisted(StoryId const& story_id) const
    {
        std::lock_guard<std::mutex> lock(mtx);
        auto iter = stories.find(story_id);
        return (iter == stories.end()) ? 0 : iter->second.w;
    }

    // Stories whose W changed since the last snapshot, with their current W;
    // clears the dirty set.
    std::map<StoryId, uint64_t> snapshotDirty()
    {
        std::lock_guard<std::mutex> lock(mtx);
        std::map<StoryId, uint64_t> snapshot;
        for(auto const& story_id: dirty)
        {
            auto iter = stories.find(story_id);
            if(iter != stories.end())
            {
                snapshot.emplace(story_id, iter->second.w);
            }
        }
        dirty.clear();
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

    mutable std::mutex mtx;
    std::map<StoryId, Entry> stories;
    std::set<StoryId> dirty;
};

} // namespace chronolog

#endif // CHRONOLOG_STORY_WATERMARK_REGISTRY_H
