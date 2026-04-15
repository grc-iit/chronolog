#ifndef CHRONOLOG_KEEPER_RUNTIME_H_
#define CHRONOLOG_KEEPER_RUNTIME_H_

#include <chimaera/chimaera.h>
#include <chimaera/container.h>
#include "keeper_tasks.h"
#include "autogen/keeper_methods.h"
#include "keeper_client.h"

#include <chronolog_types.h>
#include <StoryChunk.h>
#include <StoryPipeline.h>
#include <StoryChunkExtractionQueue.h>

#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <set>
#include <chrono>
#include <functional>

namespace chronolog::keeper {

// Per-story event ingestion state with double-buffered queues
struct StoryIngestionState {
  std::string chronicle_name;
  std::string story_name;
  chronolog::StoryId story_id;
  std::mutex ingestion_mutex;
  std::deque<chronolog::LogEvent> active_queue;   // writers push here
  std::deque<chronolog::LogEvent> passive_queue;   // monitor drains this
  chronolog::StoryPipeline* pipeline;
  std::set<chronolog::ClientId> acquirers;

  StoryIngestionState(chronolog::StoryChunkExtractionQueue& extraction_queue,
                      const std::string& chronicle, const std::string& story,
                      chronolog::StoryId sid, uint64_t start_time,
                      uint32_t chunk_duration_secs, uint32_t acceptance_window_secs)
      : chronicle_name(chronicle), story_name(story), story_id(sid),
        pipeline(new chronolog::StoryPipeline(
            extraction_queue, chronicle, story, sid,
            start_time, chunk_duration_secs, acceptance_window_secs)) {}

  ~StoryIngestionState() { delete pipeline; }

  // Double-buffer swap: only swap if passive is empty and active is not
  void swapQueues() {
    if (!passive_queue.empty() || active_queue.empty()) return;
    std::lock_guard<std::mutex> lock(ingestion_mutex);
    if (!passive_queue.empty() || active_queue.empty()) return;
    active_queue.swap(passive_queue);
  }

  // Ingest a single event (called from RecordEvent handler)
  void ingestEvent(const chronolog::LogEvent& event) {
    std::lock_guard<std::mutex> lock(ingestion_mutex);
    active_queue.push_back(event);
  }

  StoryIngestionState(const StoryIngestionState&) = delete;
  StoryIngestionState& operator=(const StoryIngestionState&) = delete;
};

class Runtime : public chi::Container {
 public:
  using CreateParams = chronolog::keeper::CreateParams;

 private:
  chi::u64 event_counter_ = 0;
  Client client_;

  // Configuration from CreateParams
  chi::u32 max_story_chunk_size_ = 4096;
  chi::u32 story_chunk_duration_secs_ = 10;
  chi::u32 acceptance_window_secs_ = 15;

  // Chronicle registry: chronicle_name -> set of story_ids
  std::mutex chronicles_mutex_;
  std::unordered_map<std::string, std::unordered_set<chronolog::StoryId>> chronicles_;

  // Active story pipelines: story_id -> ingestion state
  std::mutex pipelines_mutex_;
  std::unordered_map<chronolog::StoryId, StoryIngestionState*> active_pipelines_;

  // Pipelines scheduled for retirement: story_id -> (state, exit_time_ns)
  std::unordered_map<chronolog::StoryId,
                     std::pair<StoryIngestionState*, uint64_t>> pipelines_waiting_for_exit_;

  // Orphan events for stories without active pipelines
  std::mutex orphan_mutex_;
  std::deque<chronolog::LogEvent> orphan_events_;

  // Extraction queue for sealed story chunks
  chronolog::StoryChunkExtractionQueue extraction_queue_;

 public:
  Runtime() = default;
  virtual ~Runtime() = default;

  // Implemented in autogen/keeper_lib_exec.cc
  void Init(const chi::PoolId& pool_id, const std::string& pool_name,
            chi::u32 container_id = 0) override;

  chi::TaskResume Run(chi::u32 method, hipc::FullPtr<chi::Task> task_ptr,
                      chi::RunContext& rctx) override;

  // Method handlers
  chi::TaskResume Create(hipc::FullPtr<CreateTask> task, chi::RunContext& rctx);
  chi::TaskResume Destroy(hipc::FullPtr<DestroyTask> task, chi::RunContext& rctx);
  chi::TaskResume Monitor(hipc::FullPtr<MonitorTask> task, chi::RunContext& rctx);
  chi::TaskResume RecordEvent(hipc::FullPtr<RecordEventTask> task, chi::RunContext& rctx);
  chi::TaskResume RecordEventBatch(hipc::FullPtr<RecordEventBatchTask> task, chi::RunContext& rctx);
  chi::TaskResume StartStoryRecording(hipc::FullPtr<StartStoryRecordingTask> task, chi::RunContext& rctx);
  chi::TaskResume StopStoryRecording(hipc::FullPtr<StopStoryRecordingTask> task, chi::RunContext& rctx);
  chi::TaskResume CreateChronicle(hipc::FullPtr<CreateChronicleTask> task, chi::RunContext& rctx);
  chi::TaskResume DestroyChronicle(hipc::FullPtr<DestroyChronicleTask> task, chi::RunContext& rctx);
  chi::TaskResume AcquireStory(hipc::FullPtr<AcquireStoryTask> task, chi::RunContext& rctx);
  chi::TaskResume ReleaseStory(hipc::FullPtr<ReleaseStoryTask> task, chi::RunContext& rctx);
  chi::TaskResume ShowChronicles(hipc::FullPtr<ShowChroniclesTask> task, chi::RunContext& rctx);
  chi::TaskResume ShowStories(hipc::FullPtr<ShowStoriesTask> task, chi::RunContext& rctx);

  chi::u64 GetWorkRemaining() const override;

  // Virtual overrides implemented in autogen/keeper_lib_exec.cc
  void SaveTask(chi::u32 method, chi::SaveTaskArchive& archive,
                hipc::FullPtr<chi::Task> task_ptr) override;
  void LoadTask(chi::u32 method, chi::LoadTaskArchive& archive,
                hipc::FullPtr<chi::Task> task_ptr) override;
  hipc::FullPtr<chi::Task> AllocLoadTask(chi::u32 method,
                                          chi::LoadTaskArchive& archive) override;
  void LocalLoadTask(chi::u32 method, chi::DefaultLoadArchive& archive,
                     hipc::FullPtr<chi::Task> task_ptr) override;
  hipc::FullPtr<chi::Task> LocalAllocLoadTask(chi::u32 method,
                                               chi::DefaultLoadArchive& archive) override;
  void LocalSaveTask(chi::u32 method, chi::DefaultSaveArchive& archive,
                     hipc::FullPtr<chi::Task> task_ptr) override;
  hipc::FullPtr<chi::Task> NewCopyTask(chi::u32 method,
                                        hipc::FullPtr<chi::Task> orig_task_ptr,
                                        bool deep) override;
  hipc::FullPtr<chi::Task> NewTask(chi::u32 method) override;
  void Aggregate(chi::u32 method, hipc::FullPtr<chi::Task> orig_task,
                 const hipc::FullPtr<chi::Task>& replica_task) override;
  void DelTask(chi::u32 method, hipc::FullPtr<chi::Task> task_ptr) override;
};

}  // namespace chronolog::keeper

#endif  // CHRONOLOG_KEEPER_RUNTIME_H_
