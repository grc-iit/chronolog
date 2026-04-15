#include "chronolog/player/player_runtime.h"
#include "chronolog/player/autogen/player_methods.h"
#include <chimaera/chimaera.h>
#include <chimaera/task.h>

namespace chronolog::player {

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
    case Method::kReplayStory: {
      hipc::FullPtr<ReplayStoryTask> typed_task = task_ptr.template Cast<ReplayStoryTask>();
      CHI_CO_AWAIT(ReplayStory(typed_task, rctx));
      break;
    }
    case Method::kReplayChronicle: {
      hipc::FullPtr<ReplayChronicleTask> typed_task = task_ptr.template Cast<ReplayChronicleTask>();
      CHI_CO_AWAIT(ReplayChronicle(typed_task, rctx));
      break;
    }
    case Method::kGetStoryInfo: {
      hipc::FullPtr<GetStoryInfoTask> typed_task = task_ptr.template Cast<GetStoryInfoTask>();
      CHI_CO_AWAIT(GetStoryInfo(typed_task, rctx));
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
    case Method::kReplayStory: {
      auto typed_task = task_ptr.template Cast<ReplayStoryTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kReplayChronicle: {
      auto typed_task = task_ptr.template Cast<ReplayChronicleTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kGetStoryInfo: {
      auto typed_task = task_ptr.template Cast<GetStoryInfoTask>();
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
    case Method::kReplayStory: {
      auto typed_task = task_ptr.template Cast<ReplayStoryTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kReplayChronicle: {
      auto typed_task = task_ptr.template Cast<ReplayChronicleTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kGetStoryInfo: {
      auto typed_task = task_ptr.template Cast<GetStoryInfoTask>();
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
    case Method::kReplayStory: {
      auto typed_task = task_ptr.template Cast<ReplayStoryTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kReplayChronicle: {
      auto typed_task = task_ptr.template Cast<ReplayChronicleTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kGetStoryInfo: {
      auto typed_task = task_ptr.template Cast<GetStoryInfoTask>();
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
    case Method::kReplayStory: {
      auto typed_task = task_ptr.template Cast<ReplayStoryTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kReplayChronicle: {
      auto typed_task = task_ptr.template Cast<ReplayChronicleTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kGetStoryInfo: {
      auto typed_task = task_ptr.template Cast<GetStoryInfoTask>();
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
    case Method::kReplayStory: {
      auto new_task_ptr = ipc_manager->NewTask<ReplayStoryTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<ReplayStoryTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kReplayChronicle: {
      auto new_task_ptr = ipc_manager->NewTask<ReplayChronicleTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<ReplayChronicleTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kGetStoryInfo: {
      auto new_task_ptr = ipc_manager->NewTask<GetStoryInfoTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<GetStoryInfoTask>();
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
    case Method::kReplayStory: {
      auto new_task_ptr = ipc_manager->NewTask<ReplayStoryTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kReplayChronicle: {
      auto new_task_ptr = ipc_manager->NewTask<ReplayChronicleTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kGetStoryInfo: {
      auto new_task_ptr = ipc_manager->NewTask<GetStoryInfoTask>();
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
    case Method::kReplayStory: {
      auto typed_task = orig_task.template Cast<ReplayStoryTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kReplayChronicle: {
      auto typed_task = orig_task.template Cast<ReplayChronicleTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kGetStoryInfo: {
      auto typed_task = orig_task.template Cast<GetStoryInfoTask>();
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
    case Method::kReplayStory: {
      ipc_manager->DelTask(task_ptr.template Cast<ReplayStoryTask>());
      break;
    }
    case Method::kReplayChronicle: {
      ipc_manager->DelTask(task_ptr.template Cast<ReplayChronicleTask>());
      break;
    }
    case Method::kGetStoryInfo: {
      ipc_manager->DelTask(task_ptr.template Cast<GetStoryInfoTask>());
      break;
    }
    default: {
      ipc_manager->DelTask(task_ptr);
      break;
    }
  }
}

}  // namespace chronolog::player

// C-linkage factory functions for ChiMod dynamic loading
extern "C" {
chi::Container* alloc_chimod() { return new chronolog::player::Runtime(); }
void new_chimod(chi::Container* container) { new (container) chronolog::player::Runtime(); }
const char* get_chimod_name() { return "chronolog_player"; }
}
