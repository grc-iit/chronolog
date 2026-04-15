#include "chronolog/store/store_runtime.h"
#include "chronolog/store/autogen/store_methods.h"
#include <chimaera/chimaera.h>
#include <chimaera/task.h>

namespace chronolog::store {

//==============================================================================
// Container Virtual API Implementations
//==============================================================================

void Runtime::Init(const chi::PoolId &pool_id, const std::string &pool_name,
                   chi::u32 container_id) {
  chi::Container::Init(pool_id, pool_name, container_id);
  client_ = Client(pool_id);
  DefineModel(Method::kMaxMethodId);
  SetMethodNames(Method::GetMethodNames());
}

chi::TaskResume Runtime::Run(chi::u32 method, hipc::FullPtr<chi::Task> task_ptr, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  switch (method) {
    case Method::kCreate: {
      hipc::FullPtr<CreateTask> typed_task = task_ptr.template Cast<CreateTask>();
      CHI_CO_AWAIT(Create(typed_task, rctx));
      break;
    }
    case Method::kDestroy: {
      hipc::FullPtr<DestroyTask> typed_task = task_ptr.template Cast<DestroyTask>();
      CHI_CO_AWAIT(Destroy(typed_task, rctx));
      break;
    }
    case Method::kMonitor: {
      hipc::FullPtr<MonitorTask> typed_task = task_ptr.template Cast<MonitorTask>();
      CHI_CO_AWAIT(Monitor(typed_task, rctx));
      break;
    }
    case Method::kArchiveChunk: {
      hipc::FullPtr<ArchiveChunkTask> typed_task = task_ptr.template Cast<ArchiveChunkTask>();
      CHI_CO_AWAIT(ArchiveChunk(typed_task, rctx));
      break;
    }
    case Method::kReadChunk: {
      hipc::FullPtr<ReadChunkTask> typed_task = task_ptr.template Cast<ReadChunkTask>();
      CHI_CO_AWAIT(ReadChunk(typed_task, rctx));
      break;
    }
    case Method::kListArchives: {
      hipc::FullPtr<ListArchivesTask> typed_task = task_ptr.template Cast<ListArchivesTask>();
      CHI_CO_AWAIT(ListArchives(typed_task, rctx));
      break;
    }
    case Method::kDeleteArchive: {
      hipc::FullPtr<DeleteArchiveTask> typed_task = task_ptr.template Cast<DeleteArchiveTask>();
      CHI_CO_AWAIT(DeleteArchive(typed_task, rctx));
      break;
    }
    case Method::kCompactArchive: {
      hipc::FullPtr<CompactArchiveTask> typed_task = task_ptr.template Cast<CompactArchiveTask>();
      CHI_CO_AWAIT(CompactArchive(typed_task, rctx));
      break;
    }
    default: {
      break;
    }
  }
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

void Runtime::SaveTask(chi::u32 method, chi::SaveTaskArchive& archive,
                       hipc::FullPtr<chi::Task> task_ptr) {
  switch (method) {
    case Method::kCreate: {
      auto typed_task = task_ptr.template Cast<CreateTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kDestroy: {
      auto typed_task = task_ptr.template Cast<DestroyTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kMonitor: {
      auto typed_task = task_ptr.template Cast<MonitorTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kArchiveChunk: {
      auto typed_task = task_ptr.template Cast<ArchiveChunkTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kReadChunk: {
      auto typed_task = task_ptr.template Cast<ReadChunkTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kListArchives: {
      auto typed_task = task_ptr.template Cast<ListArchivesTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kDeleteArchive: {
      auto typed_task = task_ptr.template Cast<DeleteArchiveTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kCompactArchive: {
      auto typed_task = task_ptr.template Cast<CompactArchiveTask>();
      archive << *typed_task.ptr_;
      break;
    }
    default: {
      break;
    }
  }
}

void Runtime::LoadTask(chi::u32 method, chi::LoadTaskArchive& archive,
                       hipc::FullPtr<chi::Task> task_ptr) {
  switch (method) {
    case Method::kCreate: {
      auto typed_task = task_ptr.template Cast<CreateTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kDestroy: {
      auto typed_task = task_ptr.template Cast<DestroyTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kMonitor: {
      auto typed_task = task_ptr.template Cast<MonitorTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kArchiveChunk: {
      auto typed_task = task_ptr.template Cast<ArchiveChunkTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kReadChunk: {
      auto typed_task = task_ptr.template Cast<ReadChunkTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kListArchives: {
      auto typed_task = task_ptr.template Cast<ListArchivesTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kDeleteArchive: {
      auto typed_task = task_ptr.template Cast<DeleteArchiveTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kCompactArchive: {
      auto typed_task = task_ptr.template Cast<CompactArchiveTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    default: {
      break;
    }
  }
}

hipc::FullPtr<chi::Task> Runtime::AllocLoadTask(chi::u32 method, chi::LoadTaskArchive& archive) {
  hipc::FullPtr<chi::Task> task_ptr = NewTask(method);
  if (!task_ptr.IsNull()) {
    LoadTask(method, archive, task_ptr);
  }
  return task_ptr;
}

void Runtime::LocalLoadTask(chi::u32 method, chi::DefaultLoadArchive& archive,
                            hipc::FullPtr<chi::Task> task_ptr) {
  switch (method) {
    case Method::kCreate: {
      auto typed_task = task_ptr.template Cast<CreateTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kDestroy: {
      auto typed_task = task_ptr.template Cast<DestroyTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kMonitor: {
      auto typed_task = task_ptr.template Cast<MonitorTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kArchiveChunk: {
      auto typed_task = task_ptr.template Cast<ArchiveChunkTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kReadChunk: {
      auto typed_task = task_ptr.template Cast<ReadChunkTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kListArchives: {
      auto typed_task = task_ptr.template Cast<ListArchivesTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kDeleteArchive: {
      auto typed_task = task_ptr.template Cast<DeleteArchiveTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kCompactArchive: {
      auto typed_task = task_ptr.template Cast<CompactArchiveTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    default: {
      break;
    }
  }
}

hipc::FullPtr<chi::Task> Runtime::LocalAllocLoadTask(chi::u32 method, chi::DefaultLoadArchive& archive) {
  hipc::FullPtr<chi::Task> task_ptr = NewTask(method);
  if (!task_ptr.IsNull()) {
    LocalLoadTask(method, archive, task_ptr);
  }
  return task_ptr;
}

void Runtime::LocalSaveTask(chi::u32 method, chi::DefaultSaveArchive& archive,
                            hipc::FullPtr<chi::Task> task_ptr) {
  switch (method) {
    case Method::kCreate: {
      auto typed_task = task_ptr.template Cast<CreateTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kDestroy: {
      auto typed_task = task_ptr.template Cast<DestroyTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kMonitor: {
      auto typed_task = task_ptr.template Cast<MonitorTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kArchiveChunk: {
      auto typed_task = task_ptr.template Cast<ArchiveChunkTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kReadChunk: {
      auto typed_task = task_ptr.template Cast<ReadChunkTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kListArchives: {
      auto typed_task = task_ptr.template Cast<ListArchivesTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kDeleteArchive: {
      auto typed_task = task_ptr.template Cast<DeleteArchiveTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kCompactArchive: {
      auto typed_task = task_ptr.template Cast<CompactArchiveTask>();
      archive << *typed_task.ptr_;
      break;
    }
    default: {
      break;
    }
  }
}

hipc::FullPtr<chi::Task> Runtime::NewCopyTask(chi::u32 method, hipc::FullPtr<chi::Task> orig_task_ptr, bool deep) {
  auto* ipc_manager = CHI_IPC;
  if (!ipc_manager) {
    return hipc::FullPtr<chi::Task>();
  }

  switch (method) {
    case Method::kCreate: {
      auto new_task_ptr = ipc_manager->NewTask<CreateTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<CreateTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kDestroy: {
      auto new_task_ptr = ipc_manager->NewTask<DestroyTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<DestroyTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kMonitor: {
      auto new_task_ptr = ipc_manager->NewTask<MonitorTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<MonitorTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kArchiveChunk: {
      auto new_task_ptr = ipc_manager->NewTask<ArchiveChunkTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<ArchiveChunkTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kReadChunk: {
      auto new_task_ptr = ipc_manager->NewTask<ReadChunkTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<ReadChunkTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kListArchives: {
      auto new_task_ptr = ipc_manager->NewTask<ListArchivesTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<ListArchivesTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kDeleteArchive: {
      auto new_task_ptr = ipc_manager->NewTask<DeleteArchiveTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<DeleteArchiveTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kCompactArchive: {
      auto new_task_ptr = ipc_manager->NewTask<CompactArchiveTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<CompactArchiveTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    default: {
      auto new_task_ptr = ipc_manager->NewTask<chi::Task>();
      if (!new_task_ptr.IsNull()) {
        new_task_ptr->Copy(orig_task_ptr);
        return new_task_ptr;
      }
      break;
    }
  }

  (void)deep;
  return hipc::FullPtr<chi::Task>();
}

hipc::FullPtr<chi::Task> Runtime::NewTask(chi::u32 method) {
  auto* ipc_manager = CHI_IPC;
  if (!ipc_manager) {
    return hipc::FullPtr<chi::Task>();
  }

  switch (method) {
    case Method::kCreate: {
      auto new_task_ptr = ipc_manager->NewTask<CreateTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kDestroy: {
      auto new_task_ptr = ipc_manager->NewTask<DestroyTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kMonitor: {
      auto new_task_ptr = ipc_manager->NewTask<MonitorTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kArchiveChunk: {
      auto new_task_ptr = ipc_manager->NewTask<ArchiveChunkTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kReadChunk: {
      auto new_task_ptr = ipc_manager->NewTask<ReadChunkTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kListArchives: {
      auto new_task_ptr = ipc_manager->NewTask<ListArchivesTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kDeleteArchive: {
      auto new_task_ptr = ipc_manager->NewTask<DeleteArchiveTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kCompactArchive: {
      auto new_task_ptr = ipc_manager->NewTask<CompactArchiveTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    default: {
      return hipc::FullPtr<chi::Task>();
    }
  }
}

void Runtime::Aggregate(chi::u32 method, hipc::FullPtr<chi::Task> orig_task,
                        const hipc::FullPtr<chi::Task>& replica_task) {
  switch (method) {
    case Method::kCreate: {
      auto typed_task = orig_task.template Cast<CreateTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kDestroy: {
      auto typed_task = orig_task.template Cast<DestroyTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kMonitor: {
      auto typed_task = orig_task.template Cast<MonitorTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kArchiveChunk: {
      auto typed_task = orig_task.template Cast<ArchiveChunkTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kReadChunk: {
      auto typed_task = orig_task.template Cast<ReadChunkTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kListArchives: {
      auto typed_task = orig_task.template Cast<ListArchivesTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kDeleteArchive: {
      auto typed_task = orig_task.template Cast<DeleteArchiveTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kCompactArchive: {
      auto typed_task = orig_task.template Cast<CompactArchiveTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    default: {
      orig_task->Aggregate(replica_task);
      break;
    }
  }
}

void Runtime::DelTask(chi::u32 method, hipc::FullPtr<chi::Task> task_ptr) {
  auto* ipc_manager = CHI_IPC;
  if (!ipc_manager) return;
  switch (method) {
    case Method::kCreate: {
      ipc_manager->DelTask(task_ptr.template Cast<CreateTask>());
      break;
    }
    case Method::kDestroy: {
      ipc_manager->DelTask(task_ptr.template Cast<DestroyTask>());
      break;
    }
    case Method::kMonitor: {
      ipc_manager->DelTask(task_ptr.template Cast<MonitorTask>());
      break;
    }
    case Method::kArchiveChunk: {
      ipc_manager->DelTask(task_ptr.template Cast<ArchiveChunkTask>());
      break;
    }
    case Method::kReadChunk: {
      ipc_manager->DelTask(task_ptr.template Cast<ReadChunkTask>());
      break;
    }
    case Method::kListArchives: {
      ipc_manager->DelTask(task_ptr.template Cast<ListArchivesTask>());
      break;
    }
    case Method::kDeleteArchive: {
      ipc_manager->DelTask(task_ptr.template Cast<DeleteArchiveTask>());
      break;
    }
    case Method::kCompactArchive: {
      ipc_manager->DelTask(task_ptr.template Cast<CompactArchiveTask>());
      break;
    }
    default: {
      ipc_manager->DelTask(task_ptr);
      break;
    }
  }
}

}  // namespace chronolog::store

// C-linkage factory functions for ChiMod dynamic loading
extern "C" {
chi::Container* alloc_chimod() { return new chronolog::store::Runtime(); }
void new_chimod(chi::Container* container) { new (container) chronolog::store::Runtime(); }
const char* get_chimod_name() { return "chronolog_store"; }
}
