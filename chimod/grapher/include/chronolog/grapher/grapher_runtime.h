#ifndef CHRONOLOG_GRAPHER_RUNTIME_H_
#define CHRONOLOG_GRAPHER_RUNTIME_H_

#include <chimaera/chimaera.h>
#include <chimaera/container.h>
#include "grapher_tasks.h"
#include "autogen/grapher_methods.h"
#include "grapher_client.h"

#include <chronolog_types.h>
#include <StoryChunk.h>
#include <StoryPipeline.h>
#include <StoryChunkExtractionQueue.h>
#include <StoryChunkIngestionHandle.h>

#include <unordered_map>
#include <mutex>
#include <chrono>

namespace chronolog::grapher {

class Runtime : public chi::Container {
 public:
  using CreateParams = chronolog::grapher::CreateParams;

 private:
  Client client_;

  // Configuration
  chi::u32 merge_timeout_secs_ = 30;
  chi::u32 archive_age_threshold_secs_ = 300;

  // Per-story merge pipelines: receives chunks from keepers
  std::mutex pipelines_mutex_;
  std::unordered_map<chronolog::StoryId, chronolog::StoryPipeline*> story_pipelines_;

  // Extraction queue for merged/decayed chunks (ready for store archival)
  chronolog::StoryChunkExtractionQueue extraction_queue_;

  // Track total merge operations
  chi::u64 merge_counter_ = 0;

 public:
  Runtime() = default;
  virtual ~Runtime() = default;

  // Implemented in autogen/grapher_lib_exec.cc
  void Init(const chi::PoolId& pool_id, const std::string& pool_name,
            chi::u32 container_id = 0) override;

  chi::TaskResume Run(chi::u32 method, hipc::FullPtr<chi::Task> task_ptr,
                      chi::RunContext& rctx) override;

  // Method handlers
  chi::TaskResume Create(hipc::FullPtr<CreateTask> task, chi::RunContext& rctx);
  chi::TaskResume Destroy(hipc::FullPtr<DestroyTask> task, chi::RunContext& rctx);
  chi::TaskResume Monitor(hipc::FullPtr<MonitorTask> task, chi::RunContext& rctx);
  chi::TaskResume MergeChunks(hipc::FullPtr<MergeChunksTask> task, chi::RunContext& rctx);
  chi::TaskResume FlushStory(hipc::FullPtr<FlushStoryTask> task, chi::RunContext& rctx);
  chi::TaskResume GetMergeStatus(hipc::FullPtr<GetMergeStatusTask> task, chi::RunContext& rctx);

  chi::u64 GetWorkRemaining() const override;

  // Virtual overrides implemented in autogen/grapher_lib_exec.cc
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

}  // namespace chronolog::grapher

#endif  // CHRONOLOG_GRAPHER_RUNTIME_H_
