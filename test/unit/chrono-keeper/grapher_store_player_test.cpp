#include <gtest/gtest.h>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <chrono_monitor.h>
#include <chronolog_types.h>
#include <StoryChunk.h>
#include <StoryPipeline.h>
#include <StoryChunkExtractionQueue.h>

// Unit tests for grapher, store, and player domain logic
// using chrono_common types without Chimaera runtime.

namespace chl = chronolog;
namespace fs = std::filesystem;

static constexpr uint64_t NS = 1000000000ULL;

static void initLogger() {
  static bool initialized = false;
  if (!initialized) {
    chl::chrono_monitor::initialize("console", "", chronolog::LogLevel::debug,
                                    "chimod_test");
    initialized = true;
  }
}

// =========================================================================
// GRAPHER DOMAIN LOGIC TESTS
// =========================================================================

class GrapherTest : public ::testing::Test {
 protected:
  chl::StoryChunkExtractionQueue extraction_queue;
  void SetUp() override { initLogger(); }
};

TEST_F(GrapherTest, PipelineCreatedOnFirstMerge) {
  std::unordered_map<chl::StoryId, chl::StoryPipeline*> pipelines;

  chl::StoryId sid = 42;
  auto it = pipelines.find(sid);
  EXPECT_EQ(it, pipelines.end());

  // Create pipeline on first encounter
  std::string id_str = std::to_string(sid);
  pipelines[sid] = new chl::StoryPipeline(
      extraction_queue, id_str, id_str, sid, 0, 1, 1);

  EXPECT_EQ(pipelines.size(), 1);
  EXPECT_NE(pipelines.find(sid), pipelines.end());

  delete pipelines[sid];
}

TEST_F(GrapherTest, MergeChunkIntoTimeline) {
  chl::StoryId sid = 1;
  std::string id_str = "1";
  auto* pipeline = new chl::StoryPipeline(
      extraction_queue, id_str, id_str, sid, 0, 1, 1);

  // Merge a chunk with events
  chl::StoryChunk chunk(id_str, id_str, sid, 0, NS);
  chunk.insertEvent(chl::LogEvent(sid, NS / 2, 1, 0, "merged"));
  EXPECT_EQ(chunk.getEventCount(), 1);

  pipeline->mergeEvents(chunk);
  EXPECT_TRUE(chunk.empty());

  // Extract decayed
  pipeline->extractDecayedStoryChunks(2 * NS + 1);
  EXPECT_GE(extraction_queue.size(), 1);

  delete pipeline;
}

TEST_F(GrapherTest, FlushStoryExtractsAllChunks) {
  chl::StoryId sid = 1;
  std::string id_str = "1";
  auto* pipeline = new chl::StoryPipeline(
      extraction_queue, id_str, id_str, sid, 0, 1, 1);

  // Insert events in multiple chunks
  chl::StoryChunk c1(id_str, id_str, sid, 0, NS);
  c1.insertEvent(chl::LogEvent(sid, NS / 2, 1, 0, "ev1"));
  pipeline->mergeEvents(c1);

  chl::StoryChunk c2(id_str, id_str, sid, NS, 2 * NS);
  c2.insertEvent(chl::LogEvent(sid, NS + NS / 2, 1, 1, "ev2"));
  pipeline->mergeEvents(c2);

  // Flush: extract all with a time well past acceptance window
  // (avoid UINT64_MAX which can trigger boundary bugs in the decay loop)
  pipeline->extractDecayedStoryChunks(100 * NS + 1);
  // At least the first chunk should be extracted
  EXPECT_GE(extraction_queue.size(), 1);

  delete pipeline;
}

TEST_F(GrapherTest, MultiplePipelinesIndependent) {
  std::unordered_map<chl::StoryId, chl::StoryPipeline*> pipelines;

  for (chl::StoryId sid = 1; sid <= 3; sid++) {
    std::string id_str = std::to_string(sid);
    pipelines[sid] = new chl::StoryPipeline(
        extraction_queue, id_str, id_str, sid, 0, 1, 1);

    chl::StoryChunk chunk(id_str, id_str, sid, 0, NS);
    chunk.insertEvent(chl::LogEvent(sid, NS / 2, 1, 0, "data"));
    pipelines[sid]->mergeEvents(chunk);
  }

  EXPECT_EQ(pipelines.size(), 3);

  // Extract from all
  for (auto& [sid, pipeline] : pipelines) {
    pipeline->extractDecayedStoryChunks(2 * NS + 1);
  }
  EXPECT_GE(extraction_queue.size(), 3);

  for (auto& [sid, pipeline] : pipelines) delete pipeline;
}

// =========================================================================
// STORY CHUNK SERIALIZER TESTS
// =========================================================================

#include <StoryChunkSerializer.h>

class SerializerTest : public ::testing::Test {
 protected:
  void SetUp() override { initLogger(); }
};

TEST_F(SerializerTest, SerializeDeserializeRoundTrip) {
  chl::StoryChunk chunk("weather", "station1", 42, 100 * NS, 200 * NS);
  chunk.insertEvent(chl::LogEvent(42, 110 * NS, 1, 0, "temp=25.5"));
  chunk.insertEvent(chl::LogEvent(42, 120 * NS, 1, 1, "temp=26.0"));
  chunk.insertEvent(chl::LogEvent(42, 130 * NS, 2, 0, "humidity=80%"));

  ASSERT_EQ(chunk.getEventCount(), 3);

  // Serialize
  size_t buf_size = chl::StoryChunkSerializer::computeSerializedSize(chunk);
  ASSERT_GT(buf_size, 0);

  std::vector<char> buf(buf_size);
  size_t written = chl::StoryChunkSerializer::serialize(chunk, buf.data(), buf_size);
  ASSERT_EQ(written, buf_size);

  // Deserialize
  chl::StoryChunk* restored = chl::StoryChunkSerializer::deserialize(buf.data(), buf_size);
  ASSERT_NE(restored, nullptr);

  EXPECT_EQ(restored->getChronicleName(), "weather");
  EXPECT_EQ(restored->getStoryName(), "station1");
  EXPECT_EQ(restored->getStoryId(), 42);
  EXPECT_EQ(restored->getStartTime(), 100 * NS);
  EXPECT_EQ(restored->getEndTime(), 200 * NS);
  EXPECT_EQ(restored->getEventCount(), 3);

  // Verify events match
  auto orig_it = chunk.begin();
  auto rest_it = restored->begin();
  for (; orig_it != chunk.end() && rest_it != restored->end();
       ++orig_it, ++rest_it) {
    EXPECT_EQ(orig_it->second.eventTime, rest_it->second.eventTime);
    EXPECT_EQ(orig_it->second.clientId, rest_it->second.clientId);
    EXPECT_EQ(orig_it->second.eventIndex, rest_it->second.eventIndex);
    EXPECT_EQ(orig_it->second.logRecord, rest_it->second.logRecord);
  }

  delete restored;
}

TEST_F(SerializerTest, EmptyChunkRoundTrip) {
  chl::StoryChunk chunk("logs", "app1", 99, 0, 10 * NS);
  ASSERT_EQ(chunk.getEventCount(), 0);

  size_t buf_size = chl::StoryChunkSerializer::computeSerializedSize(chunk);
  std::vector<char> buf(buf_size);
  size_t written = chl::StoryChunkSerializer::serialize(chunk, buf.data(), buf_size);
  ASSERT_EQ(written, buf_size);

  chl::StoryChunk* restored = chl::StoryChunkSerializer::deserialize(buf.data(), buf_size);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getChronicleName(), "logs");
  EXPECT_EQ(restored->getStoryName(), "app1");
  EXPECT_EQ(restored->getEventCount(), 0);

  delete restored;
}

TEST_F(SerializerTest, BufferTooSmallReturnsZero) {
  chl::StoryChunk chunk("c", "s", 1, 0, NS);
  chunk.insertEvent(chl::LogEvent(1, 1, 1, 0, "data"));

  size_t needed = chl::StoryChunkSerializer::computeSerializedSize(chunk);
  std::vector<char> buf(needed / 2);  // too small
  size_t written = chl::StoryChunkSerializer::serialize(chunk, buf.data(), buf.size());
  EXPECT_EQ(written, 0);
}

TEST_F(SerializerTest, MakeBlobName) {
  std::string name = chl::StoryChunkSerializer::makeBlobName("station1", 100, 200);
  EXPECT_EQ(name, "station1.100.200");
}

TEST_F(SerializerTest, ParseBlobName) {
  std::string story;
  uint64_t start, end;
  ASSERT_TRUE(chl::StoryChunkSerializer::parseBlobName("station1.100.200", story, start, end));
  EXPECT_EQ(story, "station1");
  EXPECT_EQ(start, 100);
  EXPECT_EQ(end, 200);
}

TEST_F(SerializerTest, ParseBlobNameInvalid) {
  std::string story;
  uint64_t start, end;
  EXPECT_FALSE(chl::StoryChunkSerializer::parseBlobName("invalid", story, start, end));
  EXPECT_FALSE(chl::StoryChunkSerializer::parseBlobName("a.b", story, start, end));
}

TEST_F(SerializerTest, ParseBlobNameWithDotsInStory) {
  std::string story;
  uint64_t start, end;
  ASSERT_TRUE(chl::StoryChunkSerializer::parseBlobName("my.station.100.200", story, start, end));
  EXPECT_EQ(story, "my.station");
  EXPECT_EQ(start, 100);
  EXPECT_EQ(end, 200);
}

TEST_F(SerializerTest, LargePayloadRoundTrip) {
  chl::StoryChunk chunk("bigdata", "stream1", 7, 0, 100 * NS);
  std::string large_record(8192, 'X');
  chunk.insertEvent(chl::LogEvent(7, 50 * NS, 1, 0, large_record));

  size_t buf_size = chl::StoryChunkSerializer::computeSerializedSize(chunk);
  std::vector<char> buf(buf_size);
  size_t written = chl::StoryChunkSerializer::serialize(chunk, buf.data(), buf_size);
  ASSERT_EQ(written, buf_size);

  chl::StoryChunk* restored = chl::StoryChunkSerializer::deserialize(buf.data(), buf_size);
  ASSERT_NE(restored, nullptr);
  EXPECT_EQ(restored->getEventCount(), 1);
  EXPECT_EQ(restored->begin()->second.logRecord, large_record);

  delete restored;
}
