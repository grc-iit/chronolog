// Unit tests for StoryWatermarkRegistry — the grapher-side per-story persisted
// watermark W. W is the end of the longest contiguous run of persisted merged
// timeline windows anchored at the story start. The grapher's extraction module
// runs multiple drain streams, so windows of one story can persist out of
// order; a plain max(end) would report unpersisted gaps as durable.

#include <filesystem>

#include <ArchiveManifest.h>
#include <chronolog_errcode.h>
#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include <StoryWatermarkRegistry.h>

namespace chl = chronolog;

namespace
{
constexpr chl::StoryId kStory = 42;
constexpr chl::StoryId kOtherStory = 77;

// Nanosecond-scale ticks; values are arbitrary but ordered.
constexpr uint64_t T0 = 1000;
constexpr uint64_t T1 = 2000;
constexpr uint64_t T2 = 3000;
constexpr uint64_t T3 = 4000;
constexpr uint64_t T4 = 5000;
} // namespace

TEST(StoryWatermarkRegistry, UnknownStoryReportsZero)
{
    chl::StoryWatermarkRegistry registry;
    EXPECT_EQ(registry.getPersisted(kStory), 0u);
}

TEST(StoryWatermarkRegistry, RegisteredStoryAnchorsAtStartTime)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    // Nothing persisted yet, but nothing was recorded before the story start
    // either, so "everything below T0 is durable" is vacuously true.
    EXPECT_EQ(registry.getPersisted(kStory), T0);
}

TEST(StoryWatermarkRegistry, InOrderAdvance)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);

    registry.advancePersisted(kStory, T0, T1);
    EXPECT_EQ(registry.getPersisted(kStory), T1);

    registry.advancePersisted(kStory, T1, T2);
    EXPECT_EQ(registry.getPersisted(kStory), T2);
}

TEST(StoryWatermarkRegistry, FirstWindowStartsBelowAnchorStillAnchors)
{
    // The pipeline's first timeline chunk start is start_time rounded down to
    // chunk granularity, so the first persisted window may begin below the
    // registered anchor.
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0 + 500);
    registry.advancePersisted(kStory, T0, T1);
    EXPECT_EQ(registry.getPersisted(kStory), T1);
}

TEST(StoryWatermarkRegistry, OutOfOrderIntervalsWaitThenJump)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);

    // [T1,T2) persists first (second drain stream finished early): W must not
    // move past the unpersisted [T0,T1).
    registry.advancePersisted(kStory, T1, T2);
    EXPECT_EQ(registry.getPersisted(kStory), T0);

    // The missing prefix arrives: W jumps over both intervals in one step.
    registry.advancePersisted(kStory, T0, T1);
    EXPECT_EQ(registry.getPersisted(kStory), T2);
}

TEST(StoryWatermarkRegistry, GapHoldsWUntilFilled)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);

    registry.advancePersisted(kStory, T0, T1);
    registry.advancePersisted(kStory, T2, T3); // [T1,T2) missing
    EXPECT_EQ(registry.getPersisted(kStory), T1);

    registry.advancePersisted(kStory, T3, T4); // still missing
    EXPECT_EQ(registry.getPersisted(kStory), T1);

    registry.advancePersisted(kStory, T1, T2); // gap fills
    EXPECT_EQ(registry.getPersisted(kStory), T4);
}

TEST(StoryWatermarkRegistry, IntervalBelowWIsIgnoredNoRegression)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.advancePersisted(kStory, T0, T2);

    // A straggler's window re-persisted as a rotated file: already covered.
    registry.advancePersisted(kStory, T0, T1);
    EXPECT_EQ(registry.getPersisted(kStory), T2);
}

TEST(StoryWatermarkRegistry, OverlappingIntervalExtendsPrefix)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.advancePersisted(kStory, T0, T2);

    // Overlaps the persisted prefix but extends past it.
    registry.advancePersisted(kStory, T1, T3);
    EXPECT_EQ(registry.getPersisted(kStory), T3);
}

TEST(StoryWatermarkRegistry, ReRegisterKeepsWForReacquiredStory)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.advancePersisted(kStory, T0, T2);

    // Story re-acquired with the same (or earlier) start: W must not regress.
    registry.registerStory(kStory, T0);
    EXPECT_EQ(registry.getPersisted(kStory), T2);
}

TEST(StoryWatermarkRegistry, ReRegisterFreshPipelineAboveWTreatsGapAsCovered)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.advancePersisted(kStory, T0, T1);

    // Story retired cleanly (every window persisted), re-acquired much later
    // with a fresh pipeline: no events were recorded in [T1, T3) by this
    // grapher, so the gap is covered.
    registry.registerStory(kStory, T3, /*fresh_pipeline=*/true);
    EXPECT_EQ(registry.getPersisted(kStory), T3);

    registry.advancePersisted(kStory, T3, T4);
    EXPECT_EQ(registry.getPersisted(kStory), T4);
}

TEST(StoryWatermarkRegistry, ReRegisterLivePipelineNeverBumpsW)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.advancePersisted(kStory, T0, T1);

    // Story re-acquired while its pipeline is still live: the open windows in
    // (W, now) may hold received-but-unpersisted events, so W must not move.
    registry.registerStory(kStory, T3, /*fresh_pipeline=*/false);
    EXPECT_EQ(registry.getPersisted(kStory), T1);
}

TEST(StoryWatermarkRegistry, WriteFailureBlocksReRegisterBump)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.advancePersisted(kStory, T0, T1);

    // A window's HDF5 write failed: its events were received but are not on
    // disk. Covering the gap on a later fresh registration would report their
    // range as durable — forbidden; the keepers' stall re-send is the recovery.
    registry.persistFailed(kStory);
    registry.registerStory(kStory, T3, /*fresh_pipeline=*/true);
    EXPECT_EQ(registry.getPersisted(kStory), T1);
}

TEST(StoryWatermarkRegistry, ParkedIntervalsBlockReRegisterBump)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.advancePersisted(kStory, T0, T1);
    registry.advancePersisted(kStory, T2, T3); // parked: [T1,T2) missing

    // The parked interval proves a persisted-but-uncovered gap below it; a
    // bump to T4 would leap over [T1,T2) whose durability is unknown.
    registry.registerStory(kStory, T4, /*fresh_pipeline=*/true);
    EXPECT_EQ(registry.getPersisted(kStory), T1);

    // Gap fills the honest way: W catches up through the parked interval.
    registry.advancePersisted(kStory, T1, T2);
    EXPECT_EQ(registry.getPersisted(kStory), T3);
}

TEST(StoryWatermarkRegistry, AdvanceOnUnregisteredStoryAnchorsAtIntervalStart)
{
    // Defensive: an extractor persisting a window for a story the registry
    // never saw registered (adoption/recovery path) anchors at that window.
    chl::StoryWatermarkRegistry registry;
    registry.advancePersisted(kStory, T1, T2);
    EXPECT_EQ(registry.getPersisted(kStory), T2);
}

TEST(StoryWatermarkRegistry, PerStoryIsolation)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.registerStory(kOtherStory, T0);

    registry.advancePersisted(kStory, T0, T3);
    EXPECT_EQ(registry.getPersisted(kStory), T3);
    EXPECT_EQ(registry.getPersisted(kOtherStory), T0);
}

TEST(StoryWatermarkRegistry, SnapshotDirtyReturnsOnlyChangedAndClears)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, T0);
    registry.registerStory(kOtherStory, T0);

    auto initial = registry.snapshotDirty();
    // Registration marks the story dirty so the very first report goes out.
    ASSERT_EQ(initial.size(), 2u);
    EXPECT_EQ(initial.at(kStory), T0);
    EXPECT_EQ(initial.at(kOtherStory), T0);

    // Nothing changed since the snapshot: dirty set was cleared.
    EXPECT_TRUE(registry.snapshotDirty().empty());

    registry.advancePersisted(kStory, T0, T1);
    auto after_advance = registry.snapshotDirty();
    ASSERT_EQ(after_advance.size(), 1u);
    EXPECT_EQ(after_advance.at(kStory), T1);
    EXPECT_TRUE(registry.snapshotDirty().empty());

    // A parked (non-contiguous) interval does not change W: not dirty.
    registry.advancePersisted(kStory, T2, T3);
    EXPECT_TRUE(registry.snapshotDirty().empty());
}

TEST(StoryWatermarkRegistry, ConcurrentAdvanceConvergesToPrefixEnd)
{
    chl::StoryWatermarkRegistry registry;
    registry.registerStory(kStory, 0);

    // 4 threads each persist an interleaved quarter of 400 unit windows.
    constexpr uint64_t kWindows = 400;
    constexpr int kThreads = 4;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for(int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
                [&registry, t]()
                {
                    for(uint64_t w = t; w < kWindows; w += kThreads) { registry.advancePersisted(kStory, w, w + 1); }
                });
    }
    for(auto& th: threads) { th.join(); }
    EXPECT_EQ(registry.getPersisted(kStory), kWindows);
}

// ---- restoring W from the archive manifest ---------------------------------
//
// A restarted Grapher has no memory of what it persisted; without the manifest
// every story's W starts at 0 and every keeper re-sends everything it retained.
// Restoration replays the manifest's persisted intervals through advancePersisted
// rather than assigning W directly, which matters for what it preserves: the
// intervals parked above a gap. Assigning only the final W would forget them, and
// the window that later closes the gap would advance W to the gap's end instead of
// past everything already on disk -- forcing a re-send of chunks that are durable.

TEST(StoryWatermarkRegistryRestore, RestoredWatermarkMatchesThePreRestartValue)
{
    chl::StoryWatermarkRegistry before;
    before.registerStory(1, 0);
    before.advancePersisted(1, 0, 10);
    before.advancePersisted(1, 10, 20);
    before.advancePersisted(1, 20, 30);
    uint64_t const w_before = before.getPersisted(1);
    ASSERT_EQ(w_before, 30u);

    // What the manifest would hold for that story.
    std::map<chl::StoryId, std::map<uint64_t, uint64_t>> intervals;
    intervals[1] = {{0, 10}, {10, 20}, {20, 30}};

    chl::StoryWatermarkRegistry after;
    after.restoreFromManifest(intervals);

    EXPECT_EQ(after.getPersisted(1), w_before);
}

TEST(StoryWatermarkRegistryRestore, RestoreParksIntervalsAboveAGapInsteadOfLosingThem)
{
    std::map<chl::StoryId, std::map<uint64_t, uint64_t>> intervals;
    intervals[1] = {{0, 10}, {20, 30}}; // 10..20 never persisted

    chl::StoryWatermarkRegistry restored;
    restored.restoreFromManifest(intervals);
    ASSERT_EQ(restored.getPersisted(1), 10u) << "W must stop at the gap";

    // The window that closes the gap arrives after the restart. If 20..30 had been
    // forgotten, W would stop at 20 and that chunk would have to be re-sent.
    restored.advancePersisted(1, 10, 20);
    EXPECT_EQ(restored.getPersisted(1), 30u) << "the parked interval was lost on restore";
}

TEST(StoryWatermarkRegistryRestore, RestoreAnchorsAStoryAtItsOwnFirstWindow)
{
    // Stories do not start at 0; anchoring at 0 would make every story look like it
    // had an unpersisted prefix stretching back to the epoch.
    std::map<chl::StoryId, std::map<uint64_t, uint64_t>> intervals;
    intervals[7] = {{1000, 1010}, {1010, 1020}};

    chl::StoryWatermarkRegistry restored;
    restored.restoreFromManifest(intervals);

    EXPECT_EQ(restored.getPersisted(7), 1020u);
}

TEST(StoryWatermarkRegistryRestore, RestoringNothingLeavesTheRegistryEmpty)
{
    chl::StoryWatermarkRegistry restored;
    restored.restoreFromManifest({});
    EXPECT_EQ(restored.getPersisted(1), 0u);
}

TEST(StoryWatermarkRegistryRestore, RestoredStoriesArePublishedToKeepers)
{
    // The point of restoring is to tell keepers they can release what is already
    // durable, so restored stories have to come back dirty.
    std::map<chl::StoryId, std::map<uint64_t, uint64_t>> intervals;
    intervals[1] = {{0, 10}};
    intervals[2] = {{5, 25}};

    chl::StoryWatermarkRegistry restored;
    restored.restoreFromManifest(intervals);

    std::map<chl::StoryId, uint64_t> const dirty = restored.snapshotDirty();
    EXPECT_EQ(dirty.size(), 2u);
    EXPECT_EQ(dirty.at(1), 10u);
    EXPECT_EQ(dirty.at(2), 25u);
}

// The end-to-end shape: publish through a manifest, then restore from what it
// recorded and compare against the registry that did the publishing.
TEST(StoryWatermarkRegistryRestore, WatermarkSurvivesARoundTripThroughTheManifest)
{
    namespace fs = std::filesystem;
    fs::path const root = fs::temp_directory_path() / ("chronolog_w_restore_" + std::to_string(::getpid()));
    fs::create_directories(root);

    chl::StoryWatermarkRegistry before;
    chl::ArchiveManifest manifest(root.string());

    auto publish = [&](chl::StoryId sid, uint64_t start, uint64_t end, chl::ManifestState state)
    {
        chl::ArchiveManifestRecord record;
        record.chronicle = "c1";
        record.story = "s1";
        record.story_id = sid;
        record.file = (state == chl::ManifestState::EMPTY) ? "" : "c1.s1." + std::to_string(start) + ".vlen.h5";
        record.start = start;
        record.end = end;
        record.state = state;
        manifest.append(record);
        before.advancePersisted(sid, start, end);
    };

    publish(1, 0, 10, chl::ManifestState::PUBLISHED);
    publish(1, 10, 20, chl::ManifestState::EMPTY); // an idle gap still counts
    publish(1, 20, 30, chl::ManifestState::PUBLISHED);
    uint64_t const w_before = before.getPersisted(1);

    chl::ArchiveManifest reloaded(root.string());
    ASSERT_EQ(reloaded.load(), chl::CL_SUCCESS);
    chl::StoryWatermarkRegistry after;
    after.restoreFromManifest(reloaded.persistedIntervals());

    EXPECT_EQ(after.getPersisted(1), w_before);

    std::error_code ec;
    fs::remove_all(root, ec);
}
