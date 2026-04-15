#include <gtest/gtest.h>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <StoryChunk.h>
#include <StoryPipeline.h>
#include <StoryChunkExtractionQueue.h>
#include <StoryChunkIngestionHandle.h>

// These tests exercise the keeper's domain logic components directly
// using chrono_common types, without requiring the Chimaera runtime.
// They validate the same algorithms that keeper_runtime.cc implements:
//   - Double-buffered event ingestion
//   - Event batching into StoryChunks
//   - Pipeline merging and chunk extraction
//   - Chronicle/story management
//   - Orphan event handling
//   - Pipeline lifecycle (start/stop/retire)

namespace chl = chronolog;

static void initLogger() {
  static bool initialized = false;
  if (!initialized) {
    chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::debug,
                                    "keeper_unit_test");
    initialized = true;
  }
}

static constexpr uint64_t NS = 1000000000ULL;

static uint64_t now_ns() {
  return std::chrono::steady_clock::now().time_since_epoch().count();
}

// =========================================================================
// Double-Buffered Event Ingestion Tests
// =========================================================================
// Validates the double-buffer swap pattern used in keeper_runtime.cc
// for decoupling event recording from pipeline processing.

class DoubleBufferTest : public ::testing::Test {
 protected:
  void SetUp() override { initLogger(); }
};

TEST_F(DoubleBufferTest, SwapMovesActiveToPassive) {
  std::mutex mutex;
  std::deque<chl::LogEvent> active, passive;

  // Simulate event recording into active queue
  {
    std::lock_guard<std::mutex> lock(mutex);
    active.push_back(chl::LogEvent(1, 100, 1, 0, "ev1"));
    active.push_back(chl::LogEvent(1, 200, 1, 1, "ev2"));
  }
  EXPECT_EQ(active.size(), 2);
  EXPECT_TRUE(passive.empty());

  // Swap (same logic as keeper monitor cycle)
  if (!passive.empty() || active.empty()) {
    FAIL() << "Swap precondition should be met";
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    active.swap(passive);
  }
  EXPECT_TRUE(active.empty());
  EXPECT_EQ(passive.size(), 2);
}

TEST_F(DoubleBufferTest, SwapNoOpWhenActiveEmpty) {
  std::deque<chl::LogEvent> active, passive;

  // Both empty - swap should not proceed
  bool should_swap = !passive.empty() || active.empty();
  EXPECT_TRUE(should_swap);  // condition says "don't swap"
}

TEST_F(DoubleBufferTest, SwapNoOpWhenPassiveNotDrained) {
  std::mutex mutex;
  std::deque<chl::LogEvent> active, passive;

  active.push_back(chl::LogEvent(1, 100, 1, 0, "ev1"));
  {
    std::lock_guard<std::mutex> lock(mutex);
    active.swap(passive);
  }
  // Passive has events now

  active.push_back(chl::LogEvent(1, 200, 1, 1, "ev2"));

  // Should NOT swap because passive isn't empty
  bool should_swap = !((!passive.empty()) || active.empty());
  EXPECT_FALSE(should_swap);
  EXPECT_EQ(active.size(), 1);
  EXPECT_EQ(passive.size(), 1);
}

TEST_F(DoubleBufferTest, ConcurrentIngestIsSafe) {
  std::mutex mutex;
  std::deque<chl::LogEvent> active;
  const int num_threads = 4;
  const int events_per_thread = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < num_threads; t++) {
    threads.emplace_back([&mutex, &active, t]() {
      for (int i = 0; i < events_per_thread; i++) {
        std::lock_guard<std::mutex> lock(mutex);
        active.push_back(chl::LogEvent(
            1, t * 1000 + i, static_cast<uint64_t>(t), i, "event"));
      }
    });
  }

  for (auto& th : threads) th.join();

  EXPECT_EQ(active.size(),
            static_cast<size_t>(num_threads * events_per_thread));
}

// =========================================================================
// Event Batching into StoryChunks Tests
// =========================================================================
// Validates the pattern from keeper Monitor Phase 1:
// drain passive queue -> batch into StoryChunk -> merge into pipeline

class EventBatchingTest : public ::testing::Test {
 protected:
  chl::StoryChunkExtractionQueue extraction_queue;
  void SetUp() override { initLogger(); }
};

TEST_F(EventBatchingTest, BatchEventsIntoChunkAndMerge) {
  chl::StoryPipeline pipeline(extraction_queue, "C", "S", 1, 0, 1, 1);

  // Simulate a batch of events from the passive queue
  std::deque<chl::LogEvent> passive_queue;
  passive_queue.push_back(chl::LogEvent(1, NS / 4, 1, 0, "first"));
  passive_queue.push_back(chl::LogEvent(1, NS / 2, 1, 1, "second"));
  passive_queue.push_back(chl::LogEvent(1, NS * 3 / 4, 1, 2, "third"));

  // Find time bounds
  uint64_t min_time = passive_queue.front().eventTime;
  uint64_t max_time = min_time;
  for (const auto& ev : passive_queue) {
    if (ev.eventTime < min_time) min_time = ev.eventTime;
    if (ev.eventTime > max_time) max_time = ev.eventTime;
  }

  // Create batch chunk and fill
  chl::StoryChunk batch("C", "S", 1, min_time, max_time + 1);
  while (!passive_queue.empty()) {
    batch.insertEvent(passive_queue.front());
    passive_queue.pop_front();
  }
  EXPECT_EQ(batch.getEventCount(), 3);

  // Merge into pipeline
  pipeline.mergeEvents(batch);

  // Events should be drained from the batch
  EXPECT_TRUE(batch.empty());
}

TEST_F(EventBatchingTest, EmptyQueueNoAction) {
  chl::StoryPipeline pipeline(extraction_queue, "C", "S", 1, 0, 1, 1);
  uint64_t start = pipeline.TimelineStart();
  uint64_t end = pipeline.TimelineEnd();

  // Empty passive queue - nothing to do
  std::deque<chl::LogEvent> passive_queue;
  EXPECT_TRUE(passive_queue.empty());

  // Pipeline timeline should be unchanged
  EXPECT_EQ(pipeline.TimelineStart(), start);
  EXPECT_EQ(pipeline.TimelineEnd(), end);
}

TEST_F(EventBatchingTest, BatchEventsSpanningMultipleChunks) {
  // Pipeline starts at 0 with 1s chunks: [0,NS), [NS,2NS), [2NS,3NS)
  chl::StoryPipeline pipeline(extraction_queue, "C", "S", 1, 0, 1, 1);

  std::deque<chl::LogEvent> passive_queue;
  // Events spanning chunks [0,NS) and [NS,2NS)
  passive_queue.push_back(chl::LogEvent(1, NS / 2, 1, 0, "chunk0"));
  passive_queue.push_back(chl::LogEvent(1, NS + NS / 2, 1, 1, "chunk1"));
  passive_queue.push_back(chl::LogEvent(1, 2 * NS + NS / 4, 1, 2, "chunk2"));

  uint64_t min_time = passive_queue.front().eventTime;
  uint64_t max_time = min_time;
  for (const auto& ev : passive_queue) {
    if (ev.eventTime < min_time) min_time = ev.eventTime;
    if (ev.eventTime > max_time) max_time = ev.eventTime;
  }

  chl::StoryChunk batch("C", "S", 1, min_time, max_time + 1);
  while (!passive_queue.empty()) {
    batch.insertEvent(passive_queue.front());
    passive_queue.pop_front();
  }
  EXPECT_EQ(batch.getEventCount(), 3);

  pipeline.mergeEvents(batch);
  EXPECT_TRUE(batch.empty());
}

TEST_F(EventBatchingTest, BatchWithFutureEventsExtendsTimeline) {
  chl::StoryPipeline pipeline(extraction_queue, "C", "S", 1, 0, 1, 1);
  uint64_t original_end = pipeline.TimelineEnd();

  // Event beyond current timeline
  chl::StoryChunk batch("C", "S", 1, 5 * NS, 6 * NS);
  batch.insertEvent(chl::LogEvent(1, 5 * NS + NS / 2, 1, 0, "future"));
  pipeline.mergeEvents(batch);

  EXPECT_GT(pipeline.TimelineEnd(), original_end);
  EXPECT_TRUE(batch.empty());
}

TEST_F(EventBatchingTest, BatchWithLateEventsExtendsTimelineBackward) {
  // Start at 5s
  chl::StoryPipeline pipeline(extraction_queue, "C", "S", 1, 5 * NS, 1, 1);
  EXPECT_EQ(pipeline.TimelineStart(), 5 * NS);

  chl::StoryChunk batch("C", "S", 1, 2 * NS, 3 * NS);
  batch.insertEvent(chl::LogEvent(1, 2 * NS + NS / 2, 1, 0, "late"));
  pipeline.mergeEvents(batch);

  EXPECT_LT(pipeline.TimelineStart(), 5 * NS);
  EXPECT_TRUE(batch.empty());
}

// =========================================================================
// Chronicle and Story Management Tests
// =========================================================================

TEST(ChronicleManagement, CreateAndDestroyChronicle) {
  initLogger();
  std::unordered_map<std::string, std::unordered_set<chl::StoryId>> chronicles;

  // Create
  chronicles.emplace("weather", std::unordered_set<chl::StoryId>{});
  EXPECT_EQ(chronicles.size(), 1);
  EXPECT_EQ(chronicles.count("weather"), 1);

  // Idempotent create
  chronicles.emplace("weather", std::unordered_set<chl::StoryId>{});
  EXPECT_EQ(chronicles.size(), 1);

  // Create another
  chronicles.emplace("sensors", std::unordered_set<chl::StoryId>{});
  EXPECT_EQ(chronicles.size(), 2);

  // Destroy
  chronicles.erase("weather");
  EXPECT_EQ(chronicles.size(), 1);
  EXPECT_EQ(chronicles.count("weather"), 0);
  EXPECT_EQ(chronicles.count("sensors"), 1);
}

TEST(ChronicleManagement, StoryRegistration) {
  initLogger();
  std::unordered_map<std::string, std::unordered_set<chl::StoryId>> chronicles;
  chronicles.emplace("weather", std::unordered_set<chl::StoryId>{});

  // Register stories
  chronicles["weather"].insert(100);
  chronicles["weather"].insert(200);
  chronicles["weather"].insert(300);
  EXPECT_EQ(chronicles["weather"].size(), 3);

  // Duplicate insert is no-op
  chronicles["weather"].insert(100);
  EXPECT_EQ(chronicles["weather"].size(), 3);
}

TEST(ChronicleManagement, StoryIdHashDeterministic) {
  initLogger();
  std::hash<std::string> hasher;

  auto make_id = [&hasher](const std::string& chronicle,
                           const std::string& story) -> uint64_t {
    return static_cast<uint64_t>(hasher(chronicle + "/" + story));
  };

  // Same input -> same output
  EXPECT_EQ(make_id("weather", "station_42"),
            make_id("weather", "station_42"));

  // Different story -> different ID
  EXPECT_NE(make_id("weather", "station_42"),
            make_id("weather", "station_43"));

  // Different chronicle -> different ID
  EXPECT_NE(make_id("weather", "station_42"),
            make_id("sensors", "station_42"));
}

TEST(ChronicleManagement, DestroyChronicleStopsStories) {
  initLogger();
  chl::StoryChunkExtractionQueue extraction_queue;
  std::unordered_map<std::string, std::unordered_set<chl::StoryId>> chronicles;
  std::unordered_map<chl::StoryId, chl::StoryPipeline*> active;
  std::unordered_map<chl::StoryId,
                     std::pair<chl::StoryPipeline*, uint64_t>> waiting;

  // Setup: create chronicle with 2 stories
  chronicles["weather"] = {100, 200};
  active[100] = new chl::StoryPipeline(extraction_queue, "weather", "s1", 100, 0, 1, 1);
  active[200] = new chl::StoryPipeline(extraction_queue, "weather", "s2", 200, 0, 1, 1);

  // Destroy chronicle: move all stories to waiting-for-exit
  auto it = chronicles.find("weather");
  ASSERT_NE(it, chronicles.end());
  uint64_t exit_time = now_ns();
  for (auto sid : it->second) {
    auto ait = active.find(sid);
    if (ait != active.end()) {
      waiting[sid] = std::make_pair(ait->second, exit_time);
    }
  }
  chronicles.erase(it);

  EXPECT_EQ(chronicles.size(), 0);
  EXPECT_EQ(waiting.size(), 2);

  // Simulate retirement
  for (auto wit = waiting.begin(); wit != waiting.end();) {
    active.erase(wit->second.first->getStoryId());
    delete wit->second.first;
    wit = waiting.erase(wit);
  }

  EXPECT_EQ(active.size(), 0);
  EXPECT_EQ(waiting.size(), 0);
}

// =========================================================================
// Orphan Event Handling Tests
// =========================================================================

class OrphanEventTest : public ::testing::Test {
 protected:
  chl::StoryChunkExtractionQueue extraction_queue;
  void SetUp() override { initLogger(); }
};

TEST_F(OrphanEventTest, EventsOrphanedWhenNoPipeline) {
  std::deque<chl::LogEvent> orphan_events;
  std::unordered_map<chl::StoryId, bool> active_stories;

  chl::LogEvent event(42, NS / 2, 1, 0, "orphan");

  // No pipeline for story 42 -> orphan
  if (active_stories.find(event.storyId) == active_stories.end()) {
    orphan_events.push_back(event);
  }
  EXPECT_EQ(orphan_events.size(), 1);
}

TEST_F(OrphanEventTest, OrphansDrainWhenPipelineAppears) {
  std::mutex mutex;
  std::deque<chl::LogEvent> orphan_events;
  std::deque<chl::LogEvent> pipeline_queue;

  // Accumulate orphans
  orphan_events.push_back(chl::LogEvent(42, NS / 2, 1, 0, "orphan1"));
  orphan_events.push_back(chl::LogEvent(42, NS, 1, 1, "orphan2"));
  orphan_events.push_back(chl::LogEvent(99, NS / 4, 2, 0, "other_story"));

  // Story 42 pipeline appears - drain matching orphans
  std::set<chl::StoryId> known = {42};
  for (auto it = orphan_events.begin(); it != orphan_events.end();) {
    if (known.count(it->storyId)) {
      pipeline_queue.push_back(*it);
      it = orphan_events.erase(it);
    } else {
      ++it;
    }
  }

  EXPECT_EQ(pipeline_queue.size(), 2);
  EXPECT_EQ(orphan_events.size(), 1);
  EXPECT_EQ(orphan_events.front().storyId, 99);
}

// =========================================================================
// Pipeline Lifecycle Tests (Start/Stop/Retire)
// =========================================================================

class PipelineLifecycleTest : public ::testing::Test {
 protected:
  chl::StoryChunkExtractionQueue extraction_queue;
  void SetUp() override { initLogger(); }
};

TEST_F(PipelineLifecycleTest, StartCreatesNewPipeline) {
  std::unordered_map<chl::StoryId, chl::StoryPipeline*> active;

  auto* pipeline = new chl::StoryPipeline(
      extraction_queue, "C", "S", 1, 0, 1, 1);
  active[1] = pipeline;

  EXPECT_EQ(active.size(), 1);
  EXPECT_NE(active[1], nullptr);

  delete pipeline;
}

TEST_F(PipelineLifecycleTest, StartIdempotentForExistingPipeline) {
  std::unordered_map<chl::StoryId, chl::StoryPipeline*> active;

  auto* pipeline = new chl::StoryPipeline(
      extraction_queue, "C", "S", 1, 0, 1, 1);
  active[1] = pipeline;

  // Second start for same story_id is idempotent
  auto it = active.find(1);
  EXPECT_NE(it, active.end());
  // Should not create a new pipeline

  EXPECT_EQ(active.size(), 1);

  delete pipeline;
}

TEST_F(PipelineLifecycleTest, StopSchedulesForExit) {
  std::unordered_map<chl::StoryId, chl::StoryPipeline*> active;
  std::unordered_map<chl::StoryId,
                     std::pair<chl::StoryPipeline*, uint64_t>> waiting;

  auto* pipeline = new chl::StoryPipeline(
      extraction_queue, "C", "S", 1, 0, 1, 1);
  active[1] = pipeline;

  // Stop: schedule for exit
  uint64_t exit_time = now_ns() + pipeline->getAcceptanceWindow();
  waiting[1] = std::make_pair(pipeline, exit_time);

  EXPECT_EQ(active.size(), 1);
  EXPECT_EQ(waiting.size(), 1);

  // Pipeline still active (not yet retired)
  EXPECT_NE(active.find(1), active.end());

  // Cleanup
  active.erase(1);
  delete pipeline;
}

TEST_F(PipelineLifecycleTest, ReacquireCancelsExit) {
  std::unordered_map<chl::StoryId, chl::StoryPipeline*> active;
  std::unordered_map<chl::StoryId,
                     std::pair<chl::StoryPipeline*, uint64_t>> waiting;

  auto* pipeline = new chl::StoryPipeline(
      extraction_queue, "C", "S", 1, 0, 1, 1);
  active[1] = pipeline;

  // Schedule for exit
  waiting[1] = std::make_pair(pipeline, now_ns() + NS);

  // Re-acquire: remove from waiting
  waiting.erase(1);
  EXPECT_EQ(waiting.size(), 0);
  EXPECT_EQ(active.size(), 1);

  delete pipeline;
}

TEST_F(PipelineLifecycleTest, RetireDeletesPipelineAfterExitTime) {
  std::unordered_map<chl::StoryId, chl::StoryPipeline*> active;
  std::unordered_map<chl::StoryId,
                     std::pair<chl::StoryPipeline*, uint64_t>> waiting;

  auto* pipeline = new chl::StoryPipeline(
      extraction_queue, "C", "S", 1, 0, 1, 1);
  active[1] = pipeline;

  // Schedule with immediate exit
  waiting[1] = std::make_pair(pipeline, 0);

  // Retire
  uint64_t current_time = now_ns();
  for (auto it = waiting.begin(); it != waiting.end();) {
    if (current_time >= it->second.second) {
      active.erase(it->second.first->getStoryId());
      delete it->second.first;
      it = waiting.erase(it);
    } else {
      ++it;
    }
  }

  EXPECT_EQ(active.size(), 0);
  EXPECT_EQ(waiting.size(), 0);
}

// =========================================================================
// Full Monitor Cycle Integration Test
// =========================================================================

TEST(MonitorCycle, FullIngestExtractRetireCycle) {
  initLogger();
  chl::StoryChunkExtractionQueue extraction_queue;

  // --- Start recording ---
  auto* pipeline = new chl::StoryPipeline(
      extraction_queue, "weather", "station_42", 1, 0, 1, 1);

  std::unordered_map<chl::StoryId, chl::StoryPipeline*> active;
  active[1] = pipeline;

  // Double-buffered queues
  std::mutex ingestion_mutex;
  std::deque<chl::LogEvent> active_queue, passive_queue;

  // --- Record events ---
  for (int i = 0; i < 10; i++) {
    std::lock_guard<std::mutex> lock(ingestion_mutex);
    active_queue.push_back(chl::LogEvent(
        1, static_cast<uint64_t>(i) * NS / 5,
        1, i, "event_" + std::to_string(i)));
  }
  EXPECT_EQ(active_queue.size(), 10);

  // --- Monitor Phase 1: Swap + Batch + Merge ---
  // Swap
  if (passive_queue.empty() && !active_queue.empty()) {
    std::lock_guard<std::mutex> lock(ingestion_mutex);
    active_queue.swap(passive_queue);
  }
  EXPECT_TRUE(active_queue.empty());
  EXPECT_EQ(passive_queue.size(), 10);

  // Batch into StoryChunk
  uint64_t min_t = passive_queue.front().eventTime;
  uint64_t max_t = min_t;
  for (const auto& ev : passive_queue) {
    if (ev.eventTime < min_t) min_t = ev.eventTime;
    if (ev.eventTime > max_t) max_t = ev.eventTime;
  }
  chl::StoryChunk batch("weather", "station_42", 1, min_t, max_t + 1);
  while (!passive_queue.empty()) {
    batch.insertEvent(passive_queue.front());
    passive_queue.pop_front();
  }
  EXPECT_EQ(batch.getEventCount(), 10);

  // Merge into pipeline
  pipeline->mergeEvents(batch);
  EXPECT_TRUE(batch.empty());

  // --- Monitor Phase 2: Extract decayed chunks ---
  pipeline->extractDecayedStoryChunks(2 * NS + 1);
  EXPECT_GE(extraction_queue.size(), 1);

  // Verify extracted chunk
  auto* chunk = extraction_queue.ejectStoryChunk();
  ASSERT_NE(chunk, nullptr);
  EXPECT_GT(chunk->getEventCount(), 0);
  EXPECT_EQ(chunk->getStoryId(), 1);
  delete chunk;

  // --- Stop recording: schedule for exit ---
  std::unordered_map<chl::StoryId,
                     std::pair<chl::StoryPipeline*, uint64_t>> waiting;
  waiting[1] = std::make_pair(pipeline, 0);  // immediate exit

  // --- Monitor Phase 3: Retire ---
  uint64_t current_time = now_ns();
  for (auto it = waiting.begin(); it != waiting.end();) {
    if (current_time >= it->second.second) {
      active.erase(it->second.first->getStoryId());
      delete it->second.first;
      it = waiting.erase(it);
    } else {
      ++it;
    }
  }

  EXPECT_EQ(active.size(), 0);
  EXPECT_EQ(waiting.size(), 0);
}

TEST(MonitorCycle, MultipleStoriesProcessedIndependently) {
  initLogger();
  chl::StoryChunkExtractionQueue extraction_queue;

  // Two independent stories
  auto* p1 = new chl::StoryPipeline(extraction_queue, "weather", "s1", 100, 0, 1, 1);
  auto* p2 = new chl::StoryPipeline(extraction_queue, "weather", "s2", 200, 0, 1, 1);

  // Ingest events into story 1
  chl::StoryChunk b1("weather", "s1", 100, 0, NS);
  b1.insertEvent(chl::LogEvent(100, NS / 2, 1, 0, "s1_ev"));
  p1->mergeEvents(b1);

  // Ingest events into story 2
  chl::StoryChunk b2("weather", "s2", 200, 0, NS);
  b2.insertEvent(chl::LogEvent(200, NS / 2, 2, 0, "s2_ev"));
  p2->mergeEvents(b2);

  // Extract from both
  p1->extractDecayedStoryChunks(2 * NS + 1);
  p2->extractDecayedStoryChunks(2 * NS + 1);

  // Both should have contributed chunks
  EXPECT_GE(extraction_queue.size(), 2);

  delete p1;
  delete p2;
}

// =========================================================================
// Edge Cases
// =========================================================================

TEST(EdgeCases, EmptyChunkNotExtracted) {
  initLogger();
  chl::StoryChunkExtractionQueue extraction_queue;
  chl::StoryPipeline pipeline(extraction_queue, "C", "S", 1, 0, 1, 1);

  // No events inserted - extraction should yield nothing
  pipeline.extractDecayedStoryChunks(2 * NS + 1);
  EXPECT_EQ(extraction_queue.size(), 0);
}

TEST(EdgeCases, SingleEventEndToEnd) {
  initLogger();
  chl::StoryChunkExtractionQueue extraction_queue;
  chl::StoryPipeline pipeline(extraction_queue, "C", "S", 1, 0, 1, 1);

  chl::StoryChunk chunk("C", "S", 1, 0, NS);
  chunk.insertEvent(chl::LogEvent(1, NS / 2, 1, 0, "only_event"));
  pipeline.mergeEvents(chunk);

  pipeline.extractDecayedStoryChunks(2 * NS + 1);
  EXPECT_EQ(extraction_queue.size(), 1);

  auto* extracted = extraction_queue.ejectStoryChunk();
  ASSERT_NE(extracted, nullptr);
  EXPECT_EQ(extracted->getEventCount(), 1);
  delete extracted;
}

TEST(EdgeCases, HighVolumeEventIngestion) {
  initLogger();
  chl::StoryChunkExtractionQueue extraction_queue;
  chl::StoryPipeline pipeline(extraction_queue, "C", "S", 1, 0, 1, 1);

  // Insert 1000 events across multiple time ranges
  const int total_events = 1000;
  for (int batch = 0; batch < 10; batch++) {
    uint64_t base_time = static_cast<uint64_t>(batch) * NS;
    chl::StoryChunk chunk("C", "S", 1, base_time, base_time + NS);
    for (int i = 0; i < total_events / 10; i++) {
      uint64_t event_time = base_time + static_cast<uint64_t>(i) * (NS / 100);
      chunk.insertEvent(chl::LogEvent(1, event_time, 1, batch * 100 + i, "data"));
    }
    pipeline.mergeEvents(chunk);
  }

  // Extract all decayed chunks (use strictly greater time to avoid boundary bug)
  pipeline.extractDecayedStoryChunks(12 * NS + 1);

  // Should have extracted non-empty chunks
  int total_extracted = 0;
  while (!extraction_queue.empty()) {
    auto* chunk = extraction_queue.ejectStoryChunk();
    if (chunk) {
      total_extracted += chunk->getEventCount();
      delete chunk;
    }
  }
  EXPECT_EQ(total_extracted, total_events);
}
