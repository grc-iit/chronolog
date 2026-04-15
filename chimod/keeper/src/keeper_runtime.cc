#include "chronolog/keeper/keeper_runtime.h"

#include <chrono>
#include <functional>
#include <algorithm>

#include <chronolog_types.h>
#include <StoryChunk.h>

namespace chronolog::keeper {

static uint64_t now_ns() {
  return std::chrono::steady_clock::now().time_since_epoch().count();
}

// =========================================================================
// Create - Initialize container with config params
// =========================================================================
chi::TaskResume Runtime::Create(hipc::FullPtr<CreateTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  auto params = task->GetParams();
  max_story_chunk_size_ = params.max_story_chunk_size_;
  story_chunk_duration_secs_ = params.story_chunk_duration_secs_;
  acceptance_window_secs_ = params.acceptance_window_secs_;
  HLOG(kDebug, "keeper: Create pool {} chunk_duration={}s acceptance_window={}s",
       task->pool_id_, story_chunk_duration_secs_, acceptance_window_secs_);
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
    // Finalize all waiting pipelines
    for (auto& [sid, pair] : pipelines_waiting_for_exit_) {
      delete pair.first;
    }
    pipelines_waiting_for_exit_.clear();
    // Finalize all active pipelines
    for (auto& [sid, state] : active_pipelines_) {
      delete state;
    }
    active_pipelines_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(chronicles_mutex_);
    chronicles_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(orphan_mutex_);
    orphan_events_.clear();
  }
  task->return_code_ = 0;
  task->error_message_ = "";
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Monitor - Periodic maintenance: collect events, extract chunks, retire
// =========================================================================
chi::TaskResume Runtime::Monitor(hipc::FullPtr<MonitorTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN

  // Phase 0: Drain orphan events into known pipelines
  {
    std::lock_guard<std::mutex> olock(orphan_mutex_);
    std::lock_guard<std::mutex> plock(pipelines_mutex_);
    for (auto it = orphan_events_.begin(); it != orphan_events_.end();) {
      auto pipeline_it = active_pipelines_.find(it->storyId);
      if (pipeline_it != active_pipelines_.end()) {
        pipeline_it->second->ingestEvent(*it);
        it = orphan_events_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Phase 1: Collect ingested events - swap buffers and merge into timelines
  {
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (auto& [sid, state] : active_pipelines_) {
      // Swap double-buffered queues
      state->swapQueues();

      // Drain passive queue: batch events into StoryChunks and merge
      if (!state->passive_queue.empty()) {
        // Find time bounds of the batch
        uint64_t min_time = state->passive_queue.front().eventTime;
        uint64_t max_time = min_time;
        for (const auto& ev : state->passive_queue) {
          if (ev.eventTime < min_time) min_time = ev.eventTime;
          if (ev.eventTime > max_time) max_time = ev.eventTime;
        }

        // Create a temporary chunk spanning the event range
        chronolog::StoryChunk batch_chunk(
            state->chronicle_name, state->story_name, state->story_id,
            min_time, max_time + 1);
        while (!state->passive_queue.empty()) {
          batch_chunk.insertEvent(state->passive_queue.front());
          state->passive_queue.pop_front();
        }

        // Merge the batch chunk into the pipeline timeline
        if (!batch_chunk.empty()) {
          state->pipeline->mergeEvents(batch_chunk);
        }
      }
    }
  }

  // Phase 2: Extract decayed story chunks from all pipelines
  {
    uint64_t current_time = now_ns();
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (auto& [sid, state] : active_pipelines_) {
      state->pipeline->extractDecayedStoryChunks(current_time);
    }
  }

  // Phase 3: Retire pipelines that have passed their exit time
  {
    uint64_t current_time = now_ns();
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    for (auto it = pipelines_waiting_for_exit_.begin();
         it != pipelines_waiting_for_exit_.end();) {
      if (current_time >= it->second.second) {
        StoryIngestionState* state = it->second.first;
        active_pipelines_.erase(state->story_id);
        it = pipelines_waiting_for_exit_.erase(it);
        delete state;
      } else {
        ++it;
      }
    }
  }

  // TODO Phase 4: Process extraction queue - CTE PutBlob (Phase 2 integration)
  // For now, sealed chunks accumulate in extraction_queue_

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// RecordEvent - Ingest a single event into the appropriate story pipeline
// =========================================================================
chi::TaskResume Runtime::RecordEvent(hipc::FullPtr<RecordEventTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  chronolog::LogEvent event(
      task->story_id_, task->event_time_, task->client_id_,
      task->event_index_, std::string(task->log_record_.c_str()));

  {
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    auto it = active_pipelines_.find(task->story_id_);
    if (it != active_pipelines_.end()) {
      it->second->ingestEvent(event);
    } else {
      // Orphan event: story pipeline not yet active
      std::lock_guard<std::mutex> olock(orphan_mutex_);
      orphan_events_.push_back(event);
    }
  }

  event_counter_++;
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// RecordEventBatch - Placeholder for batch event recording
// =========================================================================
chi::TaskResume Runtime::RecordEventBatch(hipc::FullPtr<RecordEventBatchTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  // TODO: Implement batch recording with vector of events
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// StartStoryRecording - Create or reactivate a story pipeline
// =========================================================================
chi::TaskResume Runtime::StartStoryRecording(hipc::FullPtr<StartStoryRecordingTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());
  chronolog::StoryId story_id = task->story_id_;
  uint64_t start_time = task->start_time_;

  if (start_time == 0) {
    start_time = now_ns();
  }

  HLOG(kDebug, "keeper: StartStoryRecording chronicle={} story={} sid={} t={}",
       chronicle_name, story_name, story_id, start_time);

  std::lock_guard<std::mutex> lock(pipelines_mutex_);

  // Check if pipeline already exists
  auto it = active_pipelines_.find(story_id);
  if (it != active_pipelines_.end()) {
    // Remove from waiting-for-exit if it was scheduled for retirement
    pipelines_waiting_for_exit_.erase(story_id);
    task->SetReturnCode(0);
    CHI_CO_RETURN;
  }

  // Create new pipeline
  auto* state = new StoryIngestionState(
      extraction_queue_, chronicle_name, story_name, story_id,
      start_time, story_chunk_duration_secs_, acceptance_window_secs_);
  active_pipelines_[story_id] = state;

  // Register story in chronicle
  {
    std::lock_guard<std::mutex> clock(chronicles_mutex_);
    chronicles_[chronicle_name].insert(story_id);
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// StopStoryRecording - Schedule a pipeline for retirement
// =========================================================================
chi::TaskResume Runtime::StopStoryRecording(hipc::FullPtr<StopStoryRecordingTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  chronolog::StoryId story_id = task->story_id_;

  HLOG(kDebug, "keeper: StopStoryRecording sid={}", story_id);

  std::lock_guard<std::mutex> lock(pipelines_mutex_);
  auto it = active_pipelines_.find(story_id);
  if (it != active_pipelines_.end()) {
    uint64_t exit_time = now_ns() + it->second->pipeline->getAcceptanceWindow();
    pipelines_waiting_for_exit_[story_id] =
        std::make_pair(it->second, exit_time);
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// CreateChronicle - Register a new chronicle
// =========================================================================
chi::TaskResume Runtime::CreateChronicle(hipc::FullPtr<CreateChronicleTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string name(task->chronicle_name_.c_str());
  HLOG(kDebug, "keeper: CreateChronicle name={}", name);

  std::lock_guard<std::mutex> lock(chronicles_mutex_);
  // Insert if not already present (idempotent)
  chronicles_.emplace(name, std::unordered_set<chronolog::StoryId>{});

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// DestroyChronicle - Remove a chronicle and stop all its story pipelines
// =========================================================================
chi::TaskResume Runtime::DestroyChronicle(hipc::FullPtr<DestroyChronicleTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string name(task->chronicle_name_.c_str());
  HLOG(kDebug, "keeper: DestroyChronicle name={}", name);

  std::unordered_set<chronolog::StoryId> story_ids;
  {
    std::lock_guard<std::mutex> lock(chronicles_mutex_);
    auto it = chronicles_.find(name);
    if (it != chronicles_.end()) {
      story_ids = it->second;
      chronicles_.erase(it);
    }
  }

  // Stop all story pipelines belonging to this chronicle
  if (!story_ids.empty()) {
    std::lock_guard<std::mutex> lock(pipelines_mutex_);
    uint64_t exit_time = now_ns();
    for (auto sid : story_ids) {
      auto it = active_pipelines_.find(sid);
      if (it != active_pipelines_.end()) {
        pipelines_waiting_for_exit_[sid] =
            std::make_pair(it->second, exit_time);
      }
    }
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// AcquireStory - Hash chronicle+story names into story_id, track acquirers
// =========================================================================
chi::TaskResume Runtime::AcquireStory(hipc::FullPtr<AcquireStoryTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());

  // Hash to produce story_id (deterministic)
  std::hash<std::string> hasher;
  std::string combined = chronicle_name + "/" + story_name;
  task->story_id_ = static_cast<chi::u64>(hasher(combined));

  HLOG(kDebug, "keeper: AcquireStory chronicle={} story={} -> sid={}",
       chronicle_name, story_name, task->story_id_);

  // Ensure chronicle exists
  {
    std::lock_guard<std::mutex> lock(chronicles_mutex_);
    chronicles_[chronicle_name].insert(task->story_id_);
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ReleaseStory - Remove a client acquirer from a story
// =========================================================================
chi::TaskResume Runtime::ReleaseStory(hipc::FullPtr<ReleaseStoryTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  chronolog::StoryId story_id = task->story_id_;
  chronolog::ClientId client_id = task->client_id_;

  HLOG(kDebug, "keeper: ReleaseStory sid={} client={}", story_id, client_id);

  std::lock_guard<std::mutex> lock(pipelines_mutex_);
  auto it = active_pipelines_.find(story_id);
  if (it != active_pipelines_.end()) {
    it->second->acquirers.erase(client_id);
    // If no more acquirers, schedule for retirement
    if (it->second->acquirers.empty()) {
      uint64_t exit_time = now_ns() + it->second->pipeline->getAcceptanceWindow();
      pipelines_waiting_for_exit_[story_id] =
          std::make_pair(it->second, exit_time);
    }
  }

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ShowChronicles - Return count of registered chronicles
// =========================================================================
chi::TaskResume Runtime::ShowChronicles(hipc::FullPtr<ShowChroniclesTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::lock_guard<std::mutex> lock(chronicles_mutex_);
  task->count_ = static_cast<chi::u32>(chronicles_.size());
  for (const auto& [name, stories] : chronicles_) {
    HLOG(kDebug, "keeper: chronicle '{}' has {} stories", name, stories.size());
  }
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ShowStories - Return count of stories in a chronicle
// =========================================================================
chi::TaskResume Runtime::ShowStories(hipc::FullPtr<ShowStoriesTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::lock_guard<std::mutex> lock(chronicles_mutex_);
  auto it = chronicles_.find(chronicle_name);
  if (it != chronicles_.end()) {
    task->count_ = static_cast<chi::u32>(it->second.size());
  } else {
    task->count_ = 0;
  }
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetWorkRemaining - Report pending work for scheduler
// =========================================================================
chi::u64 Runtime::GetWorkRemaining() const {
  return event_counter_;
}

}  // namespace chronolog::keeper
