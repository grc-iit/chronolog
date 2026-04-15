#include "chronolog/store/store_runtime.h"

#include <string>
#include <functional>

#include <CteHelper.h>
#include <StoryChunkSerializer.h>

namespace chronolog::store {

// =========================================================================
// Create - Initialize container
// =========================================================================
chi::TaskResume Runtime::Create(hipc::FullPtr<CreateTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  auto params = task->GetParams();
  archive_root_ = params.archive_root_;

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
// Monitor - No-op (CTE handles its own maintenance)
// =========================================================================
chi::TaskResume Runtime::Monitor(hipc::FullPtr<MonitorTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ArchiveChunk - Demote a chunk to cold storage via CTE score reduction
// =========================================================================
chi::TaskResume Runtime::ArchiveChunk(hipc::FullPtr<ArchiveChunkTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());
  chi::u64 start_time = task->start_time_;
  chi::u64 end_time = task->end_time_;

  // Get CTE tag for this chronicle
  auto tag_id = cte_helper_.getOrCreateTag(chronicle_name);

  // The chunk should already be in CTE from keeper/grapher.
  // Fetch it and re-put at cold archival score to trigger tier demotion.
  std::string blob_name = chronolog::StoryChunkSerializer::makeBlobName(
      story_name, start_time, end_time);

  chronolog::StoryChunk* chunk = cte_helper_.getChunk(tag_id, blob_name);
  if (chunk) {
    cte_helper_.putChunk(tag_id, *chunk, 0.1f);
    delete chunk;
  }

  archive_counter_++;
  HLOG(kDebug, "store: ArchiveChunk chronicle={} story={} [{}, {}) -> CTE score=0.1",
       chronicle_name, story_name, start_time, end_time);

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ReadChunk - Query CTE for blobs matching the time range
// =========================================================================
chi::TaskResume Runtime::ReadChunk(hipc::FullPtr<ReadChunkTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());
  chi::u64 start_time = task->start_time_;
  chi::u64 end_time = task->end_time_;

  auto tag_id = cte_helper_.getOrCreateTag(chronicle_name);
  auto blobs = cte_helper_.listChunks(tag_id);

  chi::u32 match_count = 0;
  for (const auto& blob_name : blobs) {
    std::string parsed_story;
    uint64_t blob_start, blob_end;
    if (!chronolog::StoryChunkSerializer::parseBlobName(
            blob_name, parsed_story, blob_start, blob_end)) {
      continue;
    }
    if (parsed_story != story_name) continue;
    // Check overlap: blob [blob_start, blob_end) overlaps [start_time, end_time)
    if (blob_start < end_time && blob_end > start_time) {
      match_count++;
    }
  }

  task->event_count_ = match_count;
  HLOG(kDebug, "store: ReadChunk chronicle={} story={} [{}, {}) matched {} CTE blobs",
       chronicle_name, story_name, start_time, end_time, match_count);

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ListArchives - Query CTE for all tags (chronicles)
// =========================================================================
chi::TaskResume Runtime::ListArchives(hipc::FullPtr<ListArchivesTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  // CTE tags represent chronicles — query for all
  auto tag_names = cte_helper_.queryChunks(".*", ".*");
  for (const auto& name : tag_names) {
    HLOG(kDebug, "store: ListArchives blob={}", name);
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// DeleteArchive - No-op stub
// =========================================================================
chi::TaskResume Runtime::DeleteArchive(hipc::FullPtr<DeleteArchiveTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// CompactArchive - No-op stub
// =========================================================================
chi::TaskResume Runtime::CompactArchive(hipc::FullPtr<CompactArchiveTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetWorkRemaining
// =========================================================================
chi::u64 Runtime::GetWorkRemaining() const {
  return archive_counter_;
}

}  // namespace chronolog::store
