#include "chronolog/player/player_runtime.h"

#include <filesystem>
#include <string>
#include <algorithm>

namespace chronolog::player {

// =========================================================================
// Helper: scan archive directory and populate file_index_
// =========================================================================
static void scanArchiveDirectory(
    const std::string& archive_root,
    std::map<std::pair<std::string, std::string>,
             std::map<uint64_t, std::string>>& file_index) {
  namespace fs = std::filesystem;

  file_index.clear();

  if (!fs::exists(archive_root) || !fs::is_directory(archive_root)) {
    // Archive root doesn't exist yet - will be populated later
    return;
  }

  std::error_code ec;
  for (auto& entry : fs::recursive_directory_iterator(archive_root, ec)) {
    if (!entry.is_regular_file()) continue;

    std::string ext = entry.path().extension().string();
    if (ext != ".dat" && ext != ".h5") continue;

    // Filename format: chronicle.story.startSec.endSec.ext
    std::string stem = entry.path().stem().string();  // without final ext
    // But .h5 stem still has the rest; we need the full filename minus path
    std::string filename = entry.path().filename().string();

    // Split filename by '.'
    // Expected: chronicle.story.startSec.endSec.ext
    // We need at least 5 parts
    std::vector<std::string> parts;
    size_t pos = 0;
    std::string token;
    std::string tmp = filename;
    while ((pos = tmp.find('.')) != std::string::npos) {
      parts.push_back(tmp.substr(0, pos));
      tmp = tmp.substr(pos + 1);
    }
    parts.push_back(tmp);  // last part (extension)

    if (parts.size() < 5) {
      // skip malformed archive file
      continue;
    }

    // Last part is extension, second-to-last is endSec, third-to-last is startSec
    // Everything before that forms chronicle.story
    std::string extension = parts.back();
    parts.pop_back();
    std::string end_sec_str = parts.back();
    parts.pop_back();
    std::string start_sec_str = parts.back();
    parts.pop_back();

    // Remaining parts: first is chronicle, rest form story name
    if (parts.size() < 2) {
      // skip file with missing chronicle/story
      continue;
    }

    std::string chronicle_name = parts[0];
    std::string story_name = parts[1];
    // If there were extra dots in the story name, rejoin
    for (size_t i = 2; i < parts.size(); ++i) {
      story_name += "." + parts[i];
    }

    uint64_t start_time_secs = 0;
    uint64_t end_time_secs = 0;
    try {
      start_time_secs = std::stoull(start_sec_str);
      end_time_secs = std::stoull(end_sec_str);
    } catch (...) {
      // skip file with bad timestamps
      continue;
    }

    // Convert seconds to nanoseconds for index key
    uint64_t start_time_ns = start_time_secs * 1000000000ULL;

    auto key = std::make_pair(chronicle_name, story_name);
    file_index[key][start_time_ns] = entry.path().string();

    // indexed: entry -> (chronicle, story) [start - end]
  }
}

// =========================================================================
// Create - Initialize container with archive_root from params
// =========================================================================
chi::TaskResume Runtime::Create(hipc::FullPtr<CreateTask> task,
                                chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  auto params = task->GetParams();
  archive_root_ = params.archive_root_;

  HLOG(kDebug, "player: Create pool {} archive_root={}",
       task->pool_id_, archive_root_);

  // Build the initial file index
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    scanArchiveDirectory(archive_root_, file_index_);
  }

  HLOG(kInfo, "player: indexed {} story entries from archive",
       file_index_.size());

  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Destroy - Clear all state
// =========================================================================
chi::TaskResume Runtime::Destroy(hipc::FullPtr<DestroyTask> task,
                                 chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    file_index_.clear();
  }
  task->return_code_ = 0;
  task->error_message_ = "";
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Monitor - Rescan archive directory to pick up new files
// =========================================================================
chi::TaskResume Runtime::Monitor(hipc::FullPtr<MonitorTask> task,
                                 chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    scanArchiveDirectory(archive_root_, file_index_);
  }
  HLOG(kDebug, "player: Monitor rescan complete, {} story entries indexed",
       file_index_.size());
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ReplayStory - Count archive files overlapping the query time range
// =========================================================================
chi::TaskResume Runtime::ReplayStory(hipc::FullPtr<ReplayStoryTask> task,
                                     chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());
  uint64_t start_time = task->start_time_;
  uint64_t end_time = task->end_time_;

  uint32_t count = 0;
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    auto key = std::make_pair(chronicle_name, story_name);
    auto it = file_index_.find(key);
    if (it != file_index_.end()) {
      const auto& time_map = it->second;
      // upper_bound(start_time) gives the first entry with key > start_time.
      // We step back one to include the file that may contain start_time.
      auto upper = time_map.upper_bound(start_time);
      if (upper != time_map.begin()) {
        --upper;
      }
      // Count all files from upper through those whose start_time < end_time
      for (auto file_it = upper; file_it != time_map.end(); ++file_it) {
        if (file_it->first >= end_time) break;
        ++count;
      }
    }
  }

  task->event_count_ = count;
  HLOG(kDebug, "player: ReplayStory {}/{} [{}-{}] matched {} archive files",
       chronicle_name, story_name, start_time, end_time, count);

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ReplayChronicle - Aggregate file counts across all stories in a chronicle
// =========================================================================
chi::TaskResume Runtime::ReplayChronicle(
    hipc::FullPtr<ReplayChronicleTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  uint64_t start_time = task->start_time_;
  uint64_t end_time = task->end_time_;

  uint32_t total_count = 0;
  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    for (const auto& [key, time_map] : file_index_) {
      if (key.first != chronicle_name) continue;

      auto upper = time_map.upper_bound(start_time);
      if (upper != time_map.begin()) {
        --upper;
      }
      for (auto file_it = upper; file_it != time_map.end(); ++file_it) {
        if (file_it->first >= end_time) break;
        ++total_count;
      }
    }
  }

  HLOG(kDebug, "player: ReplayChronicle {} [{}-{}] matched {} archive files",
       chronicle_name, start_time, end_time, total_count);

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetStoryInfo - Find earliest and latest times for a story
// =========================================================================
chi::TaskResume Runtime::GetStoryInfo(hipc::FullPtr<GetStoryInfoTask> task,
                                      chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());

  uint64_t earliest = UINT64_MAX;
  uint64_t latest = 0;

  {
    std::lock_guard<std::mutex> lock(index_mutex_);
    auto key = std::make_pair(chronicle_name, story_name);
    auto it = file_index_.find(key);
    if (it != file_index_.end() && !it->second.empty()) {
      // Keys are start_time_ns, sorted ascending
      earliest = it->second.begin()->first;
      latest = it->second.rbegin()->first;
    }
  }

  if (earliest == UINT64_MAX) {
    earliest = 0;
  }

  task->earliest_time_ = earliest;
  task->latest_time_ = latest;

  HLOG(kDebug, "player: GetStoryInfo {}/{} earliest={} latest={}",
       chronicle_name, story_name, earliest, latest);

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetWorkRemaining - Report pending work for scheduler
// =========================================================================
chi::u64 Runtime::GetWorkRemaining() const {
  return 0;
}

}  // namespace chronolog::player
