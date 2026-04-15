#include "chronolog/grapher/grapher_runtime.h"
#include "chronolog/grapher/autogen/grapher_methods.h"
#include <chimaera/chimaera.h>
#include <chimaera/task.h>

namespace chronolog::grapher {

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
    case Method::kMergeChunks: {
      hipc::FullPtr<MergeChunksTask> typed_task = task_ptr.template Cast<MergeChunksTask>();
      CHI_CO_AWAIT(MergeChunks(typed_task, rctx));
      break;
    }
    case Method::kFlushStory: {
      hipc::FullPtr<FlushStoryTask> typed_task = task_ptr.template Cast<FlushStoryTask>();
      CHI_CO_AWAIT(FlushStory(typed_task, rctx));
      break;
    }
    case Method::kGetMergeStatus: {
      hipc::FullPtr<GetMergeStatusTask> typed_task = task_ptr.template Cast<GetMergeStatusTask>();
      CHI_CO_AWAIT(GetMergeStatus(typed_task, rctx));
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
    case Method::kMergeChunks: {
      auto typed_task = task_ptr.template Cast<MergeChunksTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kFlushStory: {
      auto typed_task = task_ptr.template Cast<FlushStoryTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kGetMergeStatus: {
      auto typed_task = task_ptr.template Cast<GetMergeStatusTask>();
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
    case Method::kMergeChunks: {
      auto typed_task = task_ptr.template Cast<MergeChunksTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kFlushStory: {
      auto typed_task = task_ptr.template Cast<FlushStoryTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kGetMergeStatus: {
      auto typed_task = task_ptr.template Cast<GetMergeStatusTask>();
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
    case Method::kMergeChunks: {
      auto typed_task = task_ptr.template Cast<MergeChunksTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kFlushStory: {
      auto typed_task = task_ptr.template Cast<FlushStoryTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kGetMergeStatus: {
      auto typed_task = task_ptr.template Cast<GetMergeStatusTask>();
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
    case Method::kMergeChunks: {
      auto typed_task = task_ptr.template Cast<MergeChunksTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kFlushStory: {
      auto typed_task = task_ptr.template Cast<FlushStoryTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kGetMergeStatus: {
      auto typed_task = task_ptr.template Cast<GetMergeStatusTask>();
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
    case Method::kMergeChunks: {
      auto new_task_ptr = ipc_manager->NewTask<MergeChunksTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<MergeChunksTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kFlushStory: {
      auto new_task_ptr = ipc_manager->NewTask<FlushStoryTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<FlushStoryTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kGetMergeStatus: {
      auto new_task_ptr = ipc_manager->NewTask<GetMergeStatusTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<GetMergeStatusTask>();
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
    case Method::kMergeChunks: {
      auto new_task_ptr = ipc_manager->NewTask<MergeChunksTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kFlushStory: {
      auto new_task_ptr = ipc_manager->NewTask<FlushStoryTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kGetMergeStatus: {
      auto new_task_ptr = ipc_manager->NewTask<GetMergeStatusTask>();
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
    case Method::kMergeChunks: {
      auto typed_task = orig_task.template Cast<MergeChunksTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kFlushStory: {
      auto typed_task = orig_task.template Cast<FlushStoryTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kGetMergeStatus: {
      auto typed_task = orig_task.template Cast<GetMergeStatusTask>();
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
    case Method::kMergeChunks: {
      ipc_manager->DelTask(task_ptr.template Cast<MergeChunksTask>());
      break;
    }
    case Method::kFlushStory: {
      ipc_manager->DelTask(task_ptr.template Cast<FlushStoryTask>());
      break;
    }
    case Method::kGetMergeStatus: {
      ipc_manager->DelTask(task_ptr.template Cast<GetMergeStatusTask>());
      break;
    }
    default: {
      ipc_manager->DelTask(task_ptr);
      break;
    }
  }
}

}  // namespace chronolog::grapher

// C-linkage factory functions for ChiMod dynamic loading
extern "C" {
chi::Container* alloc_chimod() { return new chronolog::grapher::Runtime(); }
void new_chimod(chi::Container* container) { new (container) chronolog::grapher::Runtime(); }
const char* get_chimod_name() { return "chronolog_grapher"; }
}
