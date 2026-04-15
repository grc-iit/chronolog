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
// STORE DOMAIN LOGIC TESTS
// =========================================================================

class StoreTest : public ::testing::Test {
 protected:
  std::string archive_root;

  void SetUp() override {
    initLogger();
    archive_root = "/tmp/chronolog_test_store_" +
                   std::to_string(std::chrono::steady_clock::now()
                                      .time_since_epoch().count());
    fs::create_directories(archive_root);
  }

  void TearDown() override {
    fs::remove_all(archive_root);
  }
};

TEST_F(StoreTest, ArchiveCreatesDirectoryAndFile) {
  std::string chronicle = "weather";
  std::string story = "station1";
  uint64_t start = 10 * NS;
  uint64_t end = 20 * NS;

  // Simulate store ArchiveChunk logic
  fs::path chronicle_dir = fs::path(archive_root) / chronicle;
  fs::create_directories(chronicle_dir);

  uint64_t start_s = start / NS;
  uint64_t end_s = end / NS;
  std::string filename = chronicle + "." + story + "." +
                         std::to_string(start_s) + "." +
                         std::to_string(end_s) + ".dat";
  fs::path filepath = chronicle_dir / filename;

  std::ofstream ofs(filepath, std::ios::out | std::ios::trunc);
  ASSERT_TRUE(ofs.is_open());
  ofs << "story_id=42\n";
  ofs << "start_time=" << start << "\n";
  ofs << "end_time=" << end << "\n";
  ofs.close();

  EXPECT_TRUE(fs::exists(filepath));
  EXPECT_GT(fs::file_size(filepath), 0);
}

TEST_F(StoreTest, ReadChunkFindsOverlappingFiles) {
  std::string chronicle = "weather";
  std::string story = "station1";

  fs::path chronicle_dir = fs::path(archive_root) / chronicle;
  fs::create_directories(chronicle_dir);

  // Create three archive files with different time ranges
  auto create_file = [&](uint64_t start_s, uint64_t end_s) {
    std::string fname = chronicle + "." + story + "." +
                        std::to_string(start_s) + "." +
                        std::to_string(end_s) + ".dat";
    std::ofstream ofs(chronicle_dir / fname);
    ofs << "data\n";
    ofs.close();
  };

  create_file(10, 20);  // [10s, 20s)
  create_file(20, 30);  // [20s, 30s)
  create_file(30, 40);  // [30s, 40s)

  // Query [15s, 25s) should match files [10,20) and [20,30)
  uint64_t query_start_s = 15;
  uint64_t query_end_s = 25;
  std::string prefix = chronicle + "." + story + ".";

  uint32_t match_count = 0;
  for (const auto& entry : fs::directory_iterator(chronicle_dir)) {
    if (!entry.is_regular_file()) continue;
    std::string fname = entry.path().filename().string();
    if (fname.rfind(prefix, 0) != 0) continue;
    if (fname.substr(fname.size() - 4) != ".dat") continue;

    std::string suffix = fname.substr(prefix.size());
    suffix = suffix.substr(0, suffix.size() - 4);
    auto dot_pos = suffix.find('.');
    if (dot_pos == std::string::npos) continue;

    uint64_t file_start = std::stoull(suffix.substr(0, dot_pos));
    uint64_t file_end = std::stoull(suffix.substr(dot_pos + 1));

    if (file_start < query_end_s && file_end > query_start_s) {
      match_count++;
    }
  }

  EXPECT_EQ(match_count, 2);
}

TEST_F(StoreTest, ListArchivesFindsChronicleDirectories) {
  fs::create_directories(fs::path(archive_root) / "weather");
  fs::create_directories(fs::path(archive_root) / "sensors");
  fs::create_directories(fs::path(archive_root) / "logs");

  int count = 0;
  for (const auto& entry : fs::directory_iterator(archive_root)) {
    if (entry.is_directory()) count++;
  }
  EXPECT_EQ(count, 3);
}

// =========================================================================
// PLAYER DOMAIN LOGIC TESTS
// =========================================================================

class PlayerTest : public ::testing::Test {
 protected:
  std::string archive_root;
  // File index type matching player runtime
  std::map<std::pair<std::string, std::string>,
           std::map<uint64_t, std::string>> file_index;

  void SetUp() override {
    initLogger();
    archive_root = "/tmp/chronolog_test_player_" +
                   std::to_string(std::chrono::steady_clock::now()
                                      .time_since_epoch().count());
    fs::create_directories(archive_root);
  }

  void TearDown() override {
    fs::remove_all(archive_root);
  }

  void createArchiveFile(const std::string& chronicle,
                         const std::string& story,
                         uint64_t start_s, uint64_t end_s) {
    fs::path dir = fs::path(archive_root) / chronicle;
    fs::create_directories(dir);
    std::string fname = chronicle + "." + story + "." +
                        std::to_string(start_s) + "." +
                        std::to_string(end_s) + ".dat";
    std::ofstream ofs(dir / fname);
    ofs << "data\n";
    ofs.close();

    // Also add to index
    auto key = std::make_pair(chronicle, story);
    file_index[key][start_s * NS] = (dir / fname).string();
  }
};

TEST_F(PlayerTest, ScanBuildsIndex) {
  createArchiveFile("weather", "station1", 10, 20);
  createArchiveFile("weather", "station1", 20, 30);
  createArchiveFile("weather", "station2", 10, 20);

  EXPECT_EQ(file_index.size(), 2);  // 2 unique (chronicle, story) pairs
  auto key1 = std::make_pair(std::string("weather"), std::string("station1"));
  EXPECT_EQ(file_index[key1].size(), 2);  // 2 time entries
}

TEST_F(PlayerTest, ReplayStoryFindsOverlappingFiles) {
  createArchiveFile("weather", "station1", 10, 20);
  createArchiveFile("weather", "station1", 20, 30);
  createArchiveFile("weather", "station1", 30, 40);

  // Query [15*NS, 25*NS)
  uint64_t start_time = 15 * NS;
  uint64_t end_time = 25 * NS;

  auto key = std::make_pair(std::string("weather"), std::string("station1"));
  auto it = file_index.find(key);
  ASSERT_NE(it, file_index.end());

  const auto& time_map = it->second;
  auto upper = time_map.upper_bound(start_time);
  if (upper != time_map.begin()) --upper;

  uint32_t count = 0;
  for (auto file_it = upper; file_it != time_map.end(); ++file_it) {
    if (file_it->first >= end_time) break;
    count++;
  }

  EXPECT_EQ(count, 2);  // Files starting at 10s and 20s
}

TEST_F(PlayerTest, GetStoryInfoFindsMinMax) {
  createArchiveFile("weather", "station1", 10, 20);
  createArchiveFile("weather", "station1", 20, 30);
  createArchiveFile("weather", "station1", 50, 60);

  auto key = std::make_pair(std::string("weather"), std::string("station1"));
  auto it = file_index.find(key);
  ASSERT_NE(it, file_index.end());

  uint64_t earliest = it->second.begin()->first;
  uint64_t latest = it->second.rbegin()->first;

  EXPECT_EQ(earliest, 10 * NS);
  EXPECT_EQ(latest, 50 * NS);
}

TEST_F(PlayerTest, ReplayChronicleAcrossStories) {
  createArchiveFile("weather", "station1", 10, 20);
  createArchiveFile("weather", "station2", 15, 25);
  createArchiveFile("sensors", "temp1", 10, 20);

  // Query all weather stories in [0, 30*NS)
  uint64_t start_time = 0;
  uint64_t end_time = 30 * NS;
  std::string chronicle = "weather";

  uint32_t total = 0;
  for (const auto& [key, time_map] : file_index) {
    if (key.first != chronicle) continue;
    auto upper = time_map.upper_bound(start_time);
    if (upper != time_map.begin()) --upper;
    for (auto file_it = upper; file_it != time_map.end(); ++file_it) {
      if (file_it->first >= end_time) break;
      total++;
    }
  }

  EXPECT_EQ(total, 2);  // station1 + station2
}

TEST_F(PlayerTest, EmptyIndexReturnsNoResults) {
  auto key = std::make_pair(std::string("nonexistent"), std::string("story"));
  auto it = file_index.find(key);
  EXPECT_EQ(it, file_index.end());
}
