#ifndef CHRONOLOG_PLAYER_RUNTIME_H_
#define CHRONOLOG_PLAYER_RUNTIME_H_

#include <chimaera/chimaera.h>
#include <chimaera/container.h>
#include "player_tasks.h"
#include "autogen/player_methods.h"
#include "player_client.h"

#include <chronolog_types.h>
#include <StoryChunk.h>

#include <string>
#include <map>
#include <mutex>
#include <filesystem>

namespace chronolog::player {

class Runtime : public chi::Container {
 public:
  using CreateParams = chronolog::player::CreateParams;

 private:
  Client client_;

  // Archive root directory for HDF5 files
  std::string archive_root_;

  // File index: (chronicle, story) -> map<start_time_ns, filepath>
  std::mutex index_mutex_;
  std::map<std::pair<std::string, std::string>,
           std::map<uint64_t, std::string>> file_index_;

 public:
  Runtime() = default;
  virtual ~Runtime() = default;

  // Implemented in autogen/player_lib_exec.cc
  void Init(const chi::PoolId& pool_id, const std::string& pool_name,
            chi::u32 container_id = 0) override;

  chi::TaskResume Run(chi::u32 method, hipc::FullPtr<chi::Task> task_ptr,
                      chi::RunContext& rctx) override;

  // Method handlers
  chi::TaskResume Create(hipc::FullPtr<CreateTask> task, chi::RunContext& rctx);
  chi::TaskResume Destroy(hipc::FullPtr<DestroyTask> task, chi::RunContext& rctx);
  chi::TaskResume Monitor(hipc::FullPtr<MonitorTask> task, chi::RunContext& rctx);
  chi::TaskResume ReplayStory(hipc::FullPtr<ReplayStoryTask> task, chi::RunContext& rctx);
  chi::TaskResume ReplayChronicle(hipc::FullPtr<ReplayChronicleTask> task, chi::RunContext& rctx);
  chi::TaskResume GetStoryInfo(hipc::FullPtr<GetStoryInfoTask> task, chi::RunContext& rctx);

  chi::u64 GetWorkRemaining() const override;

  // Virtual overrides implemented in autogen/player_lib_exec.cc
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

}  // namespace chronolog::player

#endif  // CHRONOLOG_PLAYER_RUNTIME_H_
