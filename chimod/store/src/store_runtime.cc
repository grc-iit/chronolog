#include "chronolog/store/store_runtime.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <functional>

namespace chronolog::store {

// =========================================================================
// Create - Initialize container with archive root directory
// =========================================================================
chi::TaskResume Runtime::Create(hipc::FullPtr<CreateTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  auto params = task->GetParams();
  archive_root_ = params.archive_root_;

  // Create the archive root directory if it doesn't exist
  std::filesystem::create_directories(archive_root_);

  HLOG(kDebug, "store: Create pool {} archive_root={}",
       task->pool_id_, archive_root_);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Destroy - Clean up state
// =========================================================================
chi::TaskResume Runtime::Destroy(hipc::FullPtr<DestroyTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  archive_root_.clear();
  archive_counter_ = 0;
  task->return_code_ = 0;
  task->error_message_ = "";
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Monitor - No-op for now (future: scan for stale archives)
// =========================================================================
chi::TaskResume Runtime::Monitor(hipc::FullPtr<MonitorTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  // TODO: Scan for stale archives and trigger compaction
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ArchiveChunk - Write a StoryChunk metadata file to the archive directory
// =========================================================================
chi::TaskResume Runtime::ArchiveChunk(hipc::FullPtr<ArchiveChunkTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());
  chi::u64 story_id = task->story_id_;
  chi::u64 start_time = task->start_time_;
  chi::u64 end_time = task->end_time_;

  // Construct chronicle subdirectory: archive_root_ / chronicle_name
  std::filesystem::path chronicle_dir =
      std::filesystem::path(archive_root_) / chronicle_name;
  std::filesystem::create_directories(chronicle_dir);

  // Construct filename: chronicle_name.story_name.{start_s}.{end_s}.dat
  chi::u64 start_s = start_time / 1000000000;
  chi::u64 end_s = end_time / 1000000000;
  std::string filename = chronicle_name + "." + story_name + "."
      + std::to_string(start_s) + "." + std::to_string(end_s) + ".dat";
  std::filesystem::path filepath = chronicle_dir / filename;

  // Phase 1: Write a simple text metadata file
  // (Phase 2 will replace this with HDF5 via StoryChunkWriter)
  std::ofstream ofs(filepath, std::ios::out | std::ios::trunc);
  if (ofs.is_open()) {
    ofs << "story_id=" << story_id << "\n";
    ofs << "start_time=" << start_time << "\n";
    ofs << "end_time=" << end_time << "\n";
    ofs.close();
  }

  archive_counter_++;
  HLOG(kDebug, "store: ArchiveChunk chronicle={} story={} sid={} [{}, {}) -> {}",
       chronicle_name, story_name, story_id, start_time, end_time,
       filepath.string());

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ReadChunk - Scan archive directory for files matching the time range
// =========================================================================
chi::TaskResume Runtime::ReadChunk(hipc::FullPtr<ReadChunkTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());
  chi::u64 start_time = task->start_time_;
  chi::u64 end_time = task->end_time_;

  std::filesystem::path chronicle_dir =
      std::filesystem::path(archive_root_) / chronicle_name;

  chi::u32 match_count = 0;

  // Scan for files matching this story whose time range overlaps [start_time, end_time)
  if (std::filesystem::exists(chronicle_dir) &&
      std::filesystem::is_directory(chronicle_dir)) {
    // Expected filename pattern: chronicle_name.story_name.{start_s}.{end_s}.dat
    std::string prefix = chronicle_name + "." + story_name + ".";
    chi::u64 query_start_s = start_time / 1000000000;
    chi::u64 query_end_s = end_time / 1000000000;

    for (const auto& entry : std::filesystem::directory_iterator(chronicle_dir)) {
      if (!entry.is_regular_file()) continue;

      std::string fname = entry.path().filename().string();
      if (fname.rfind(prefix, 0) != 0) continue;
      if (fname.size() < 5 || fname.substr(fname.size() - 4) != ".dat") continue;

      // Parse the time range from the filename
      // Format after prefix: {start_s}.{end_s}.dat
      std::string suffix = fname.substr(prefix.size());
      // Remove trailing ".dat"
      suffix = suffix.substr(0, suffix.size() - 4);

      // Split on '.'
      auto dot_pos = suffix.find('.');
      if (dot_pos == std::string::npos) continue;

      chi::u64 file_start_s = std::stoull(suffix.substr(0, dot_pos));
      chi::u64 file_end_s = std::stoull(suffix.substr(dot_pos + 1));

      // Check overlap: file range [file_start_s, file_end_s) overlaps [query_start_s, query_end_s)
      if (file_start_s < query_end_s && file_end_s > query_start_s) {
        match_count++;
      }
    }
  }

  task->event_count_ = match_count;
  HLOG(kDebug, "store: ReadChunk chronicle={} story={} [{}, {}) matched {} files",
       chronicle_name, story_name, start_time, end_time, match_count);

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ListArchives - Scan archive root for chronicle directories
// =========================================================================
chi::TaskResume Runtime::ListArchives(hipc::FullPtr<ListArchivesTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN

  if (std::filesystem::exists(archive_root_) &&
      std::filesystem::is_directory(archive_root_)) {
    for (const auto& entry : std::filesystem::directory_iterator(archive_root_)) {
      if (entry.is_directory()) {
        HLOG(kDebug, "store: ListArchives chronicle={}",
             entry.path().filename().string());
      }
    }
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// DeleteArchive - No-op stub for now
// =========================================================================
chi::TaskResume Runtime::DeleteArchive(hipc::FullPtr<DeleteArchiveTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  // TODO: Implement archive deletion by chronicle/story name
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// CompactArchive - No-op stub for now
// =========================================================================
chi::TaskResume Runtime::CompactArchive(hipc::FullPtr<CompactArchiveTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  // TODO: Implement archive compaction (merge small files)
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetWorkRemaining - Report pending archive operations for scheduler
// =========================================================================
chi::u64 Runtime::GetWorkRemaining() const {
  return archive_counter_;
}

}  // namespace chronolog::store
