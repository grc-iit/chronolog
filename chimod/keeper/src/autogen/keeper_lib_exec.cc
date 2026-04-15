#include "chronolog/keeper/keeper_runtime.h"
#include "chronolog/keeper/autogen/keeper_methods.h"
#include <chimaera/chimaera.h>
#include <chimaera/task.h>

namespace chronolog::keeper {

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
    case Method::kRecordEvent: {
      hipc::FullPtr<RecordEventTask> typed_task = task_ptr.template Cast<RecordEventTask>();
      CHI_CO_AWAIT(RecordEvent(typed_task, rctx));
      break;
    }
    case Method::kRecordEventBatch: {
      hipc::FullPtr<RecordEventBatchTask> typed_task = task_ptr.template Cast<RecordEventBatchTask>();
      CHI_CO_AWAIT(RecordEventBatch(typed_task, rctx));
      break;
    }
    case Method::kStartStoryRecording: {
      hipc::FullPtr<StartStoryRecordingTask> typed_task = task_ptr.template Cast<StartStoryRecordingTask>();
      CHI_CO_AWAIT(StartStoryRecording(typed_task, rctx));
      break;
    }
    case Method::kStopStoryRecording: {
      hipc::FullPtr<StopStoryRecordingTask> typed_task = task_ptr.template Cast<StopStoryRecordingTask>();
      CHI_CO_AWAIT(StopStoryRecording(typed_task, rctx));
      break;
    }
    case Method::kCreateChronicle: {
      hipc::FullPtr<CreateChronicleTask> typed_task = task_ptr.template Cast<CreateChronicleTask>();
      CHI_CO_AWAIT(CreateChronicle(typed_task, rctx));
      break;
    }
    case Method::kDestroyChronicle: {
      hipc::FullPtr<DestroyChronicleTask> typed_task = task_ptr.template Cast<DestroyChronicleTask>();
      CHI_CO_AWAIT(DestroyChronicle(typed_task, rctx));
      break;
    }
    case Method::kAcquireStory: {
      hipc::FullPtr<AcquireStoryTask> typed_task = task_ptr.template Cast<AcquireStoryTask>();
      CHI_CO_AWAIT(AcquireStory(typed_task, rctx));
      break;
    }
    case Method::kReleaseStory: {
      hipc::FullPtr<ReleaseStoryTask> typed_task = task_ptr.template Cast<ReleaseStoryTask>();
      CHI_CO_AWAIT(ReleaseStory(typed_task, rctx));
      break;
    }
    case Method::kShowChronicles: {
      hipc::FullPtr<ShowChroniclesTask> typed_task = task_ptr.template Cast<ShowChroniclesTask>();
      CHI_CO_AWAIT(ShowChronicles(typed_task, rctx));
      break;
    }
    case Method::kShowStories: {
      hipc::FullPtr<ShowStoriesTask> typed_task = task_ptr.template Cast<ShowStoriesTask>();
      CHI_CO_AWAIT(ShowStories(typed_task, rctx));
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
    case Method::kRecordEvent: {
      auto typed_task = task_ptr.template Cast<RecordEventTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kRecordEventBatch: {
      auto typed_task = task_ptr.template Cast<RecordEventBatchTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kStartStoryRecording: {
      auto typed_task = task_ptr.template Cast<StartStoryRecordingTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kStopStoryRecording: {
      auto typed_task = task_ptr.template Cast<StopStoryRecordingTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kCreateChronicle: {
      auto typed_task = task_ptr.template Cast<CreateChronicleTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kDestroyChronicle: {
      auto typed_task = task_ptr.template Cast<DestroyChronicleTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kAcquireStory: {
      auto typed_task = task_ptr.template Cast<AcquireStoryTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kReleaseStory: {
      auto typed_task = task_ptr.template Cast<ReleaseStoryTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kShowChronicles: {
      auto typed_task = task_ptr.template Cast<ShowChroniclesTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kShowStories: {
      auto typed_task = task_ptr.template Cast<ShowStoriesTask>();
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
    case Method::kRecordEvent: {
      auto typed_task = task_ptr.template Cast<RecordEventTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kRecordEventBatch: {
      auto typed_task = task_ptr.template Cast<RecordEventBatchTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kStartStoryRecording: {
      auto typed_task = task_ptr.template Cast<StartStoryRecordingTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kStopStoryRecording: {
      auto typed_task = task_ptr.template Cast<StopStoryRecordingTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kCreateChronicle: {
      auto typed_task = task_ptr.template Cast<CreateChronicleTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kDestroyChronicle: {
      auto typed_task = task_ptr.template Cast<DestroyChronicleTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kAcquireStory: {
      auto typed_task = task_ptr.template Cast<AcquireStoryTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kReleaseStory: {
      auto typed_task = task_ptr.template Cast<ReleaseStoryTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kShowChronicles: {
      auto typed_task = task_ptr.template Cast<ShowChroniclesTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kShowStories: {
      auto typed_task = task_ptr.template Cast<ShowStoriesTask>();
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
    case Method::kRecordEvent: {
      auto typed_task = task_ptr.template Cast<RecordEventTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kRecordEventBatch: {
      auto typed_task = task_ptr.template Cast<RecordEventBatchTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kStartStoryRecording: {
      auto typed_task = task_ptr.template Cast<StartStoryRecordingTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kStopStoryRecording: {
      auto typed_task = task_ptr.template Cast<StopStoryRecordingTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kCreateChronicle: {
      auto typed_task = task_ptr.template Cast<CreateChronicleTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kDestroyChronicle: {
      auto typed_task = task_ptr.template Cast<DestroyChronicleTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kAcquireStory: {
      auto typed_task = task_ptr.template Cast<AcquireStoryTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kReleaseStory: {
      auto typed_task = task_ptr.template Cast<ReleaseStoryTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kShowChronicles: {
      auto typed_task = task_ptr.template Cast<ShowChroniclesTask>();
      archive >> *typed_task.ptr_;
      break;
    }
    case Method::kShowStories: {
      auto typed_task = task_ptr.template Cast<ShowStoriesTask>();
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
    case Method::kRecordEvent: {
      auto typed_task = task_ptr.template Cast<RecordEventTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kRecordEventBatch: {
      auto typed_task = task_ptr.template Cast<RecordEventBatchTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kStartStoryRecording: {
      auto typed_task = task_ptr.template Cast<StartStoryRecordingTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kStopStoryRecording: {
      auto typed_task = task_ptr.template Cast<StopStoryRecordingTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kCreateChronicle: {
      auto typed_task = task_ptr.template Cast<CreateChronicleTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kDestroyChronicle: {
      auto typed_task = task_ptr.template Cast<DestroyChronicleTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kAcquireStory: {
      auto typed_task = task_ptr.template Cast<AcquireStoryTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kReleaseStory: {
      auto typed_task = task_ptr.template Cast<ReleaseStoryTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kShowChronicles: {
      auto typed_task = task_ptr.template Cast<ShowChroniclesTask>();
      archive << *typed_task.ptr_;
      break;
    }
    case Method::kShowStories: {
      auto typed_task = task_ptr.template Cast<ShowStoriesTask>();
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
    case Method::kRecordEvent: {
      auto new_task_ptr = ipc_manager->NewTask<RecordEventTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<RecordEventTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kRecordEventBatch: {
      auto new_task_ptr = ipc_manager->NewTask<RecordEventBatchTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<RecordEventBatchTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kStartStoryRecording: {
      auto new_task_ptr = ipc_manager->NewTask<StartStoryRecordingTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<StartStoryRecordingTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kStopStoryRecording: {
      auto new_task_ptr = ipc_manager->NewTask<StopStoryRecordingTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<StopStoryRecordingTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kCreateChronicle: {
      auto new_task_ptr = ipc_manager->NewTask<CreateChronicleTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<CreateChronicleTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kDestroyChronicle: {
      auto new_task_ptr = ipc_manager->NewTask<DestroyChronicleTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<DestroyChronicleTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kAcquireStory: {
      auto new_task_ptr = ipc_manager->NewTask<AcquireStoryTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<AcquireStoryTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kReleaseStory: {
      auto new_task_ptr = ipc_manager->NewTask<ReleaseStoryTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<ReleaseStoryTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kShowChronicles: {
      auto new_task_ptr = ipc_manager->NewTask<ShowChroniclesTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<ShowChroniclesTask>();
        new_task_ptr->Copy(task_typed);
        return new_task_ptr.template Cast<chi::Task>();
      }
      break;
    }
    case Method::kShowStories: {
      auto new_task_ptr = ipc_manager->NewTask<ShowStoriesTask>();
      if (!new_task_ptr.IsNull()) {
        auto task_typed = orig_task_ptr.template Cast<ShowStoriesTask>();
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
    case Method::kRecordEvent: {
      auto new_task_ptr = ipc_manager->NewTask<RecordEventTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kRecordEventBatch: {
      auto new_task_ptr = ipc_manager->NewTask<RecordEventBatchTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kStartStoryRecording: {
      auto new_task_ptr = ipc_manager->NewTask<StartStoryRecordingTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kStopStoryRecording: {
      auto new_task_ptr = ipc_manager->NewTask<StopStoryRecordingTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kCreateChronicle: {
      auto new_task_ptr = ipc_manager->NewTask<CreateChronicleTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kDestroyChronicle: {
      auto new_task_ptr = ipc_manager->NewTask<DestroyChronicleTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kAcquireStory: {
      auto new_task_ptr = ipc_manager->NewTask<AcquireStoryTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kReleaseStory: {
      auto new_task_ptr = ipc_manager->NewTask<ReleaseStoryTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kShowChronicles: {
      auto new_task_ptr = ipc_manager->NewTask<ShowChroniclesTask>();
      return new_task_ptr.template Cast<chi::Task>();
    }
    case Method::kShowStories: {
      auto new_task_ptr = ipc_manager->NewTask<ShowStoriesTask>();
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
    case Method::kRecordEvent: {
      auto typed_task = orig_task.template Cast<RecordEventTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kRecordEventBatch: {
      auto typed_task = orig_task.template Cast<RecordEventBatchTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kStartStoryRecording: {
      auto typed_task = orig_task.template Cast<StartStoryRecordingTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kStopStoryRecording: {
      auto typed_task = orig_task.template Cast<StopStoryRecordingTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kCreateChronicle: {
      auto typed_task = orig_task.template Cast<CreateChronicleTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kDestroyChronicle: {
      auto typed_task = orig_task.template Cast<DestroyChronicleTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kAcquireStory: {
      auto typed_task = orig_task.template Cast<AcquireStoryTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kReleaseStory: {
      auto typed_task = orig_task.template Cast<ReleaseStoryTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kShowChronicles: {
      auto typed_task = orig_task.template Cast<ShowChroniclesTask>();
      typed_task->Aggregate(replica_task);
      break;
    }
    case Method::kShowStories: {
      auto typed_task = orig_task.template Cast<ShowStoriesTask>();
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
    case Method::kRecordEvent: {
      ipc_manager->DelTask(task_ptr.template Cast<RecordEventTask>());
      break;
    }
    case Method::kRecordEventBatch: {
      ipc_manager->DelTask(task_ptr.template Cast<RecordEventBatchTask>());
      break;
    }
    case Method::kStartStoryRecording: {
      ipc_manager->DelTask(task_ptr.template Cast<StartStoryRecordingTask>());
      break;
    }
    case Method::kStopStoryRecording: {
      ipc_manager->DelTask(task_ptr.template Cast<StopStoryRecordingTask>());
      break;
    }
    case Method::kCreateChronicle: {
      ipc_manager->DelTask(task_ptr.template Cast<CreateChronicleTask>());
      break;
    }
    case Method::kDestroyChronicle: {
      ipc_manager->DelTask(task_ptr.template Cast<DestroyChronicleTask>());
      break;
    }
    case Method::kAcquireStory: {
      ipc_manager->DelTask(task_ptr.template Cast<AcquireStoryTask>());
      break;
    }
    case Method::kReleaseStory: {
      ipc_manager->DelTask(task_ptr.template Cast<ReleaseStoryTask>());
      break;
    }
    case Method::kShowChronicles: {
      ipc_manager->DelTask(task_ptr.template Cast<ShowChroniclesTask>());
      break;
    }
    case Method::kShowStories: {
      ipc_manager->DelTask(task_ptr.template Cast<ShowStoriesTask>());
      break;
    }
    default: {
      ipc_manager->DelTask(task_ptr);
      break;
    }
  }
}

}  // namespace chronolog::keeper

// C-linkage factory functions for ChiMod dynamic loading
extern "C" {
chi::Container* alloc_chimod() { return new chronolog::keeper::Runtime(); }
void new_chimod(chi::Container* container) { new (container) chronolog::keeper::Runtime(); }
const char* get_chimod_name() { return "chronolog_keeper"; }
}
