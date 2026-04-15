#include "chronolog/grapher/grapher_runtime.h"

#include <chrono>
#include <chronolog_types.h>
#include <StoryChunk.h>
#include <CteHelper.h>
#include <StoryChunkSerializer.h>

namespace chronolog::grapher {

static uint64_t now_ns() {
  return std::chrono::steady_clock::now().time_since_epoch().count();
}

// =========================================================================
// Create - Initialize grapher with config params
// =========================================================================
chi::TaskResume Runtime::Create(hipc::FullPtr<CreateTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  auto params = task->GetParams();
  merge_timeout_secs_ = params.merge_timeout_secs_;
  archive_age_threshold_secs_ = params.archive_age_threshold_secs_;
  HLOG(kDebug, "grapher: Create pool {} merge_timeout={}s archive_age={}s",
       task->pool_id_, merge_timeout_secs_, archive_age_threshold_secs_);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Destroy - Clean up all pipelines and state
// =========================================================================
chi::TaskResume Runtime::Destroy(hipc::FullPtr<DestroyTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  {
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (auto& [sid, pipeline] : story_pipelines_) {
      delete pipeline;
    }
    story_pipelines_.clear();
  }
  task->return_code_ = 0;
  task->error_message_ = "";
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Monitor - Periodic maintenance: extract decayed chunks for archival
// =========================================================================
chi::TaskResume Runtime::Monitor(hipc::FullPtr<MonitorTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN

  // Extract decayed story chunks from all pipelines
  {
    uint64_t current_time = now_ns();
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (auto& [sid, pipeline] : story_pipelines_) {
      pipeline->extractDecayedStoryChunks(current_time);
    }
  }

  // Forward sealed chunks from extraction_queue_ to CTE at warm score
  {
    while (!extraction_queue_.empty()) {
      chronolog::StoryChunk* chunk = extraction_queue_.ejectStoryChunk();
      if (!chunk) break;
      if (!chunk->empty()) {
        std::string chronicle = chunk->getChronicleName();
        auto tag_it = tag_cache_.find(chronicle);
        if (tag_it == tag_cache_.end()) {
          auto tag_id = cte_helper_.getOrCreateTag(chronicle);
          tag_cache_[chronicle] = tag_id;
          tag_it = tag_cache_.find(chronicle);
        }
        cte_helper_.putChunk(tag_it->second, *chunk, 0.5f);
        HLOG(kDebug, "grapher: CTE PutBlob (warm) chronicle={} story={} [{}-{}]",
             chronicle, chunk->getStoryName(),
             chunk->getStartTime(), chunk->getEndTime());
      }
      delete chunk;
    }
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// MergeChunks - Receive a chunk merge window from a keeper
// =========================================================================
chi::TaskResume Runtime::MergeChunks(hipc::FullPtr<MergeChunksTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  chronolog::StoryId story_id = task->story_id_;
  uint64_t window_start = task->window_start_;
  uint64_t window_end = task->window_end_;

  HLOG(kDebug, "grapher: MergeChunks sid={} window=[{}, {})",
       story_id, window_start, window_end);

  std::lock_guard<std::mutex> lock(pipelines_mutex_);

  // Find or create pipeline for this story
  auto it = story_pipelines_.find(story_id);
  if (it == story_pipelines_.end()) {
    // Create a new pipeline for this story
    // Use story_id as both chronicle/story name placeholder since the
    // grapher receives pre-formed chunks and does not have the names
    std::string id_str = std::to_string(story_id);
    auto* pipeline = new chronolog::StoryPipeline(
        extraction_queue_,
        id_str,               // chronicle_name
        id_str,               // story_name
        story_id,
        window_start,
        merge_timeout_secs_,
        archive_age_threshold_secs_);
    story_pipelines_[story_id] = pipeline;
    it = story_pipelines_.find(story_id);
  }

  // Fetch chunk from CTE (written by keeper at hot score)
  {
    std::string id_str = std::to_string(story_id);
    std::string blob_name = chronolog::StoryChunkSerializer::makeBlobName(
        id_str, window_start, window_end);

    auto tag_it = tag_cache_.find(id_str);
    if (tag_it == tag_cache_.end()) {
      auto tag_id = cte_helper_.getOrCreateTag(id_str);
      tag_cache_[id_str] = tag_id;
      tag_it = tag_cache_.find(id_str);
    }

    chronolog::StoryChunk* fetched = cte_helper_.getChunk(tag_it->second, blob_name);
    if (fetched && !fetched->empty()) {
      it->second->mergeEvents(*fetched);
    }
    delete fetched;
  }

  // Extract any chunks that have decayed past the acceptance window
  it->second->extractDecayedStoryChunks(now_ns());

  task->merged_blob_id_ = merge_counter_++;
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// FlushStory - Finalize a story pipeline and extract all remaining chunks
// =========================================================================
chi::TaskResume Runtime::FlushStory(hipc::FullPtr<FlushStoryTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  chronolog::StoryId story_id = task->story_id_;

  HLOG(kDebug, "grapher: FlushStory sid={}", story_id);

  std::lock_guard<std::mutex> lock(pipelines_mutex_);
  auto it = story_pipelines_.find(story_id);
  if (it != story_pipelines_.end()) {
    // Extract all remaining chunks before destroying the pipeline
    it->second->extractDecayedStoryChunks(
        std::numeric_limits<uint64_t>::max());

    // Flush extracted chunks to CTE at cold/archive score
    while (!extraction_queue_.empty()) {
      chronolog::StoryChunk* chunk = extraction_queue_.ejectStoryChunk();
      if (!chunk) break;
      if (!chunk->empty()) {
        std::string chronicle = chunk->getChronicleName();
        auto tag_it = tag_cache_.find(chronicle);
        if (tag_it == tag_cache_.end()) {
          auto tag_id = cte_helper_.getOrCreateTag(chronicle);
          tag_cache_[chronicle] = tag_id;
          tag_it = tag_cache_.find(chronicle);
        }
        cte_helper_.putChunk(tag_it->second, *chunk, 0.2f);
      }
      delete chunk;
    }

    delete it->second;
    story_pipelines_.erase(it);
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetMergeStatus - Return count of chunks pending in extraction queue
// =========================================================================
chi::TaskResume Runtime::GetMergeStatus(hipc::FullPtr<GetMergeStatusTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  task->pending_merges_ = static_cast<chi::u32>(extraction_queue_.size());
  HLOG(kDebug, "grapher: GetMergeStatus pending={}", task->pending_merges_);
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetWorkRemaining - Report pending work for scheduler
// =========================================================================
chi::u64 Runtime::GetWorkRemaining() const {
  return merge_counter_;
}

}  // namespace chronolog::grapher
