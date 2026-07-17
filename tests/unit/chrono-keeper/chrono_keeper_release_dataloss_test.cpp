// Reproduction for the "release-before-decay" data-loss bug on the keeper.
//
// Contract (see KeeperDataStore::stopStoryRecording): releasing a story does
// NOT tear the pipeline down immediately. The pipeline is kept alive for an
// `acceptance_window` grace period so that (a) in-flight / straggler events
// that arrive shortly after the release still land in the live ingestion
// handle and get persisted, and (b) a re-acquire can resume recording. At the
// end of the grace period the pipeline is retired and finalize() flushes every
// remaining non-empty chunk to the extraction queue.
//
// The bug: KeeperStoryPipeline::getAcceptanceWindow() returns the acceptance
// window as `uint16_t`, but the value is stored in NANOSECONDS (seconds * 1e9)
// as a uint64_t. The getter therefore truncates e.g. 60 s (60'000'000'000 ns)
// down to `60'000'000'000 % 65536` ns (~tens of microseconds). Because
// stopStoryRecording() computes `exit_time = now + getAcceptanceWindow()`, the
// pipeline is scheduled to retire almost immediately instead of after the
// configured grace period. Any event that arrives after that near-instant
// retirement is orphaned in the IngestionQueue and never persisted -> lost.
//
// These tests drive the keeper data path entirely in-process (no daemons /
// network), mirroring chrono_keeper_tail_store_test.cpp.
//
// Expected result WITH the bug present: both tests FAIL, demonstrating the loss.
// Expected result AFTER fixing getAcceptanceWindow() to return uint64_t: PASS.

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <abt.h>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <StoryChunk.h>

#include <IngestionQueue.h>
#include <KeeperDataStore.h>
#include <KeeperStoryPipeline.h>
#include <KeeperTailStore.h>
#include <StoryChunkExtractionQueue.h>

namespace chl = chronolog;

static constexpr uint64_t NS = 1000000000ULL;

static void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::err, "release_dataloss_test_logger");
        done = true;
    }
}

// KeeperDataStore's maintenance methods log via tl::thread::self_id(), which
// requires an initialized Argobots runtime (normally provided by the keeper's
// thallium execution streams). Initialize it once for the standalone test.
static void ensureArgobots()
{
    static bool done = false;
    if(!done)
    {
        ABT_init(0, nullptr);
        done = true;
    }
}

static uint64_t nowNs() { return std::chrono::high_resolution_clock::now().time_since_epoch().count(); }

// Drain the extraction queue and return the total number of events that were
// actually handed off for persistence (RDMA to grapher / CSV archival).
static int countPersistedEvents(chl::StoryChunkExtractionQueue& eq)
{
    int total = 0;
    while(chl::StoryChunk* chunk = eq.ejectStoryChunk())
    {
        total += chunk->getEventCount();
        delete chunk;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Root cause: the acceptance-window getter truncates the nanosecond value.
// ---------------------------------------------------------------------------
TEST(KeeperReleaseDataLoss, AcceptanceWindowGetterDoesNotTruncate)
{
    ensureLogger();
    chl::StoryChunkExtractionQueue eq;
    chl::KeeperTailStore tail(eq, 1024);

    const uint32_t acceptance_window_secs = 60;
    chl::KeeperStoryPipeline pipeline(eq,
                                      tail,
                                      "chron",
                                      "story",
                                      /*story_id=*/1,
                                      /*start_time=*/nowNs(),
                                      /*chunk_granularity=*/30,
                                      acceptance_window_secs);

    const uint64_t expected_ns = static_cast<uint64_t>(acceptance_window_secs) * NS;

    // With the uint16_t getter, this collapses 60e9 ns to ~tens of microseconds.
    EXPECT_EQ(static_cast<uint64_t>(pipeline.getAcceptanceWindow()), expected_ns)
            << "getAcceptanceWindow() truncated the " << acceptance_window_secs << "s (" << expected_ns
            << " ns) grace window down to " << static_cast<uint64_t>(pipeline.getAcceptanceWindow())
            << " ns. exit_time = now + this value, so the pipeline retires almost immediately after release.";
}

// ---------------------------------------------------------------------------
// Consequence: an event that arrives within the acceptance window after the
// story is released is dropped, because the pipeline was already retired.
// ---------------------------------------------------------------------------
TEST(KeeperReleaseDataLoss, EventArrivingWithinGraceWindowAfterReleaseIsPersisted)
{
    ensureLogger();
    ensureArgobots();

    chl::IngestionQueue ingestionQueue;
    chl::StoryChunkExtractionQueue extractionQueue;
    chl::KeeperTailStore tailStore(extractionQueue, 1024);

    // 1s grace window: long enough that a straggler arriving ~100ms after
    // release must still be captured; short enough to keep the test fast.
    const uint32_t acceptance_window_secs = 1;
    chl::KeeperDataStore store(ingestionQueue,
                               extractionQueue,
                               tailStore,
                               /*max_chunk_size=*/4096,
                               /*story_chunk_duration_secs=*/30,
                               acceptance_window_secs,
                               /*inactive_pipeline_delay_secs=*/300);

    const chl::StoryId sid = 42;
    const uint64_t base = nowNs();

    store.startStoryRecording("chron", "story", sid, base, /*granularity=*/30, /*window=*/acceptance_window_secs);

    // Event #1: logged while the story is actively recorded.
    ingestionQueue.ingestLogEvent(chl::LogEvent(sid, base + 1, /*client=*/1, /*idx=*/0, "before-release"));

    // Client releases the story. The pipeline should now stay alive for
    // `acceptance_window_secs` before being retired.
    store.stopStoryRecording(sid);

    // Simulate the daemon's maintenance loop running shortly after release.
    // With a correct grace window the pipeline is NOT retired here.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    store.collectIngestedEvents();
    store.extractDecayedStoryChunks();
    store.retireDecayedPipelines();

    // Event #2: an in-flight / straggler event that reaches the keeper ~100ms
    // after the release RPC -- well within the 1s acceptance window. It must
    // still be captured and persisted.
    ingestionQueue.ingestLogEvent(chl::LogEvent(sid, base + 2, /*client=*/1, /*idx=*/1, "straggler-within-window"));

    // Let the (intended) grace window elapse, then run maintenance again so the
    // pipeline is retired and everything remaining is flushed via finalize().
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    store.collectIngestedEvents();
    store.extractDecayedStoryChunks();
    store.retireDecayedPipelines();

    const int persisted = countPersistedEvents(extractionQueue);

    EXPECT_EQ(persisted, 2) << "Ingested 2 events (one before release, one straggler within the "
                            << acceptance_window_secs << "s acceptance window) but only " << persisted
                            << " reached the extraction queue. The pipeline was retired almost "
                               "immediately after release (uint16 acceptance-window truncation), "
                               "so the straggler was orphaned in the IngestionQueue and lost.";
}

// ---------------------------------------------------------------------------
// Root cause #2 (orphan queue not drained at retirement): an event parked in
// IngestionQueue::orphanEventQueue must be drainOrphanEvents()'d inside
// retireDecayedPipelines() before removeIngestionHandle(). finalize() only
// drains the handle's active/passive deques, never the orphan queue.
//
// This helper drives that path with acceptance_window_secs=0 so retirement is
// immediate (no sleeps). Returns how many events reached the extraction queue.
// WITHOUT the retirement-path drain fix: returns 1 (only the handle-deque event).
// WITH the fix: returns 1 + orphan_count.
static int persistOrphansThroughImmediateRetirement(chl::KeeperDataStore& store,
                                                    chl::IngestionQueue& ingestionQueue,
                                                    chl::StoryChunkExtractionQueue& extractionQueue,
                                                    chl::StoryId story_id,
                                                    uint64_t base_time,
                                                    int orphan_count)
{
    for(int i = 0; i < orphan_count; ++i)
    {
        ingestionQueue.ingestLogEvent(chl::LogEvent(story_id,
                                                    base_time + static_cast<uint64_t>(i + 1),
                                                    /*client=*/1,
                                                    /*idx=*/static_cast<uint32_t>(i),
                                                    "orphan-before-handle"));
    }

    store.startStoryRecording("chron", "story", story_id, base_time, /*granularity=*/30, /*window=*/0);

    // Lands in the live ingestion handle deque (not merged into the timeline yet).
    ingestionQueue.ingestLogEvent(chl::LogEvent(story_id,
                                                base_time + static_cast<uint64_t>(orphan_count + 1),
                                                /*client=*/1,
                                                /*idx=*/static_cast<uint32_t>(orphan_count),
                                                "in-handle-deque"));

    store.stopStoryRecording(story_id);

    // Deliberately skip collectIngestedEvents(): the only rescue for orphans is
    // drainOrphanEvents() inside retireDecayedPipelines().
    store.retireDecayedPipelines();

    return countPersistedEvents(extractionQueue);
}

// Single-shot, timing-free reproduction of root cause #2.
// Expected WITHOUT retireDecayedPipelines() orphan drain: FAIL (persisted == 1).
// Expected WITH the fix: PASS (persisted == 4).
TEST(KeeperOrphanAtRetirementDataLoss, OrphanQueueNotDrainedAtRetirementLosesOrphans)
{
    ensureLogger();
    ensureArgobots();

    chl::IngestionQueue ingestionQueue;
    chl::StoryChunkExtractionQueue extractionQueue;
    chl::KeeperTailStore tailStore(extractionQueue, 1024);

    const int orphan_count = 3;
    chl::KeeperDataStore store(ingestionQueue,
                               extractionQueue,
                               tailStore,
                               /*max_chunk_size=*/4096,
                               /*story_chunk_duration_secs=*/30,
                               /*acceptance_window_secs=*/0,
                               /*inactive_pipeline_delay_secs=*/300);

    const int persisted = persistOrphansThroughImmediateRetirement(store,
                                                                   ingestionQueue,
                                                                   extractionQueue,
                                                                   /*story_id=*/901,
                                                                   /*base_time=*/nowNs(),
                                                                   orphan_count);

    EXPECT_EQ(persisted, 1 + orphan_count)
            << "Ingested " << orphan_count << " orphan(s) plus 1 in-handle event but only " << persisted
            << " reached the extraction queue. retireDecayedPipelines() removed "
               "the ingestion handle without draining the orphan queue, so orphan "
               "events were stranded and lost.";
}

// Same root cause as above, exercised on every iteration with a fresh story id.
// No sleeps; acceptance_window=0 forces immediate retirement each time.
TEST(KeeperOrphanAtRetirementDataLoss, OrphanQueueNotDrainedAtRetirementReproducesEveryIteration)
{
    ensureLogger();
    ensureArgobots();

    chl::IngestionQueue ingestionQueue;
    chl::StoryChunkExtractionQueue extractionQueue;
    chl::KeeperTailStore tailStore(extractionQueue, 1024);

    constexpr int iterations = 100;
    constexpr int orphan_count = 2;

    chl::KeeperDataStore store(ingestionQueue,
                               extractionQueue,
                               tailStore,
                               /*max_chunk_size=*/4096,
                               /*story_chunk_duration_secs=*/30,
                               /*acceptance_window_secs=*/0,
                               /*inactive_pipeline_delay_secs=*/300);

    const uint64_t base = nowNs();

    for(int iter = 0; iter < iterations; ++iter)
    {
        const chl::StoryId sid = static_cast<chl::StoryId>(1000 + iter);
        const uint64_t story_base = base + static_cast<uint64_t>(iter) * 1000;

        const int persisted = persistOrphansThroughImmediateRetirement(store,
                                                                       ingestionQueue,
                                                                       extractionQueue,
                                                                       sid,
                                                                       story_base,
                                                                       orphan_count);

        ASSERT_EQ(persisted, 1 + orphan_count) << "iteration " << iter << " (story_id=" << sid << "): expected "
                                               << (1 + orphan_count) << " persisted events but got " << persisted
                                               << ". Orphans were not drained from IngestionQueue before handle "
                                                  "removal at retirement.";
    }
}

// ---------------------------------------------------------------------------
// Post-handle-removal race: an event that is parked in the IngestionQueue's
// orphan queue (an out-of-order arrival whose record_event RPC reached the
// keeper before start_story_recording registered the story's handle) must be
// drained into the pipeline before the pipeline's handle is disengaged at
// retirement.
//
// retireDecayedPipelines() erases the pipeline, calls removeIngestionHandle(),
// then delete pipeline -> finalize(). finalize() only drains the handle's own
// active/passive deques; it never touches the IngestionQueue's orphan queue.
// Unlike the grapher's destroyStory() (which calls drainOrphanChunks() +
// collectIngestedEvents() before removing the handle), the keeper's retirement
// path does no such drain -- so an orphaned event is stranded with no handle to
// ever drain into and is lost.
//
// This test deliberately does NOT call collectIngestedEvents() after creating
// the orphan, so the only chance to rescue it is inside the retirement path.
//
// Expected WITHOUT the retirement-path drain fix: FAIL (persisted == 1).
// Expected WITH the fix: PASS (persisted == 2).
TEST(KeeperReleaseDataLoss, OrphanedEventIsDrainedBeforeHandleRemovalAtRetirement)
{
    ensureLogger();
    ensureArgobots();

    chl::IngestionQueue ingestionQueue;
    chl::StoryChunkExtractionQueue extractionQueue;
    chl::KeeperTailStore tailStore(extractionQueue, 1024);

    const uint32_t acceptance_window_secs = 1;
    chl::KeeperDataStore store(ingestionQueue,
                               extractionQueue,
                               tailStore,
                               /*max_chunk_size=*/4096,
                               /*story_chunk_duration_secs=*/30,
                               acceptance_window_secs,
                               /*inactive_pipeline_delay_secs=*/300);

    const chl::StoryId sid = 77;
    const uint64_t base = nowNs();

    // Out-of-order arrival: the event's record_event RPC reaches the keeper
    // before start_story_recording has registered the story's ingestion handle,
    // so ingestLogEvent parks it in the orphan queue.
    ingestionQueue.ingestLogEvent(chl::LogEvent(sid, base + 1, /*client=*/1, /*idx=*/0, "arrived-before-start"));

    store.startStoryRecording("chron", "story", sid, base, /*granularity=*/30, /*window=*/acceptance_window_secs);

    // A normal event that lands in the live ingestion handle's deque.
    ingestionQueue.ingestLogEvent(chl::LogEvent(sid, base + 2, /*client=*/1, /*idx=*/1, "normal"));

    store.stopStoryRecording(sid);

    // Let the grace window elapse and retire the pipeline. Note: no
    // collectIngestedEvents() is called here, so the orphaned event can only be
    // rescued if retireDecayedPipelines() drains the orphan queue before
    // disengaging the handle.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    store.extractDecayedStoryChunks();
    store.retireDecayedPipelines();

    const int persisted = countPersistedEvents(extractionQueue);

    EXPECT_EQ(persisted, 2) << "Ingested 2 events (one parked in the orphan queue before the "
                               "handle was registered, one in the live deque) but only "
                            << persisted
                            << " reached the extraction queue. The retirement path removed the "
                               "ingestion handle without draining the orphan queue, so the "
                               "orphaned event was stranded and lost.";
}
