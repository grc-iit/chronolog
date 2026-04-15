#ifndef CHRONOLOG_STORE_RUNTIME_H_
#define CHRONOLOG_STORE_RUNTIME_H_

#include <chimaera/chimaera.h>
#include <chimaera/container.h>
#include "store_tasks.h"
#include "autogen/store_methods.h"
#include "store_client.h"

#include <chronolog_types.h>
#include <StoryChunk.h>
#include <CteHelper.h>

#include <string>
#include <mutex>

namespace chronolog::store {

class Runtime : public chi::Container {
 public:
  using CreateParams = chronolog::store::CreateParams;

 private:
  Client client_;

  // Archive root directory
  std::string archive_root_;

  // Track archive operations
  chi::u64 archive_counter_ = 0;

  // CTE integration
  chronolog::CteHelper cte_helper_;

 public:
  Runtime() = default;
  virtual ~Runtime() = default;

  // Implemented in autogen/store_lib_exec.cc
  void Init(const chi::PoolId& pool_id, const std::string& pool_name,
            chi::u32 container_id = 0) override;

  chi::TaskResume Run(chi::u32 method, hipc::FullPtr<chi::Task> task_ptr,
                      chi::RunContext& rctx) override;

  // Method handlers
  chi::TaskResume Create(hipc::FullPtr<CreateTask> task, chi::RunContext& rctx);
  chi::TaskResume Destroy(hipc::FullPtr<DestroyTask> task, chi::RunContext& rctx);
  chi::TaskResume Monitor(hipc::FullPtr<MonitorTask> task, chi::RunContext& rctx);
  chi::TaskResume ArchiveChunk(hipc::FullPtr<ArchiveChunkTask> task, chi::RunContext& rctx);
  chi::TaskResume ReadChunk(hipc::FullPtr<ReadChunkTask> task, chi::RunContext& rctx);
  chi::TaskResume ListArchives(hipc::FullPtr<ListArchivesTask> task, chi::RunContext& rctx);
  chi::TaskResume DeleteArchive(hipc::FullPtr<DeleteArchiveTask> task, chi::RunContext& rctx);
  chi::TaskResume CompactArchive(hipc::FullPtr<CompactArchiveTask> task, chi::RunContext& rctx);

  chi::u64 GetWorkRemaining() const override;

  // Virtual overrides implemented in autogen/store_lib_exec.cc
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

}  // namespace chronolog::store

#endif  // CHRONOLOG_STORE_RUNTIME_H_
