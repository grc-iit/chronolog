#ifndef CHRONOLOG_KEEPER_CLIENT_H_
#define CHRONOLOG_KEEPER_CLIENT_H_

#include <chimaera/chimaera.h>
#include "keeper_tasks.h"

namespace chronolog::keeper {

class Client : public chi::ContainerClient {
 public:
  HSHM_CROSS_FUN Client() = default;

  HSHM_CROSS_FUN explicit Client(const chi::PoolId& pool_id) { Init(pool_id); }

  chi::Future<CreateTask> AsyncCreate(const chi::PoolQuery& pool_query,
                                      const std::string& pool_name,
                                      const chi::PoolId& custom_pool_id) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<CreateTask>(
        chi::CreateTaskId(),
        chi::kAdminPoolId,
        pool_query,
        CreateParams::chimod_lib_name,
        pool_name,
        custom_pool_id,
        this);
    return ipc_manager->Send(task);
  }

  chi::Future<MonitorTask> AsyncMonitor(const chi::PoolQuery& pool_query,
                                        const std::string& query) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<MonitorTask>(
        chi::CreateTaskId(), pool_id_, pool_query, query);
    return ipc_manager->Send(task);
  }

  chi::Future<RecordEventTask> AsyncRecordEvent(
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      chi::u64 event_time,
      chi::u64 client_id,
      chi::u32 event_index,
      const std::string& log_record) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<RecordEventTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        story_id, event_time, client_id, event_index, log_record);
    return ipc_manager->Send(task);
  }

  chi::Future<AcquireStoryTask> AsyncAcquireStory(
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name,
      const std::string& story_name) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<AcquireStoryTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        chronicle_name, story_name);
    return ipc_manager->Send(task);
  }

  chi::Future<ReleaseStoryTask> AsyncReleaseStory(
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      chi::u64 client_id) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<ReleaseStoryTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        story_id, client_id);
    return ipc_manager->Send(task);
  }

  chi::Future<CreateChronicleTask> AsyncCreateChronicle(
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<CreateChronicleTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        chronicle_name);
    return ipc_manager->Send(task);
  }

  chi::Future<DestroyChronicleTask> AsyncDestroyChronicle(
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<DestroyChronicleTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        chronicle_name);
    return ipc_manager->Send(task);
  }

  chi::Future<StartStoryRecordingTask> AsyncStartStoryRecording(
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      const std::string& chronicle_name,
      const std::string& story_name,
      chi::u64 start_time) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<StartStoryRecordingTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        story_id, chronicle_name, story_name, start_time);
    return ipc_manager->Send(task);
  }

  chi::Future<StopStoryRecordingTask> AsyncStopStoryRecording(
      const chi::PoolQuery& pool_query,
      chi::u64 story_id) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<StopStoryRecordingTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        story_id);
    return ipc_manager->Send(task);
  }

  chi::Future<ShowChroniclesTask> AsyncShowChronicles(
      const chi::PoolQuery& pool_query) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<ShowChroniclesTask>(
        chi::CreateTaskId(), pool_id_, pool_query);
    return ipc_manager->Send(task);
  }

  chi::Future<ShowStoriesTask> AsyncShowStories(
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<ShowStoriesTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        chronicle_name);
    return ipc_manager->Send(task);
  }
};

}  // namespace chronolog::keeper

#endif  // CHRONOLOG_KEEPER_CLIENT_H_
