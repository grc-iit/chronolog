#ifndef CHRONOLOG_PLAYER_CLIENT_H_
#define CHRONOLOG_PLAYER_CLIENT_H_

#include <chimaera/chimaera.h>
#include "player_tasks.h"

namespace chronolog::player {

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

  chi::Future<ReplayStoryTask> AsyncReplayStory(
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name,
      const std::string& story_name,
      chi::u64 start_time,
      chi::u64 end_time,
      hipc::ShmPtr<> event_data,
      chi::u64 buffer_size) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<ReplayStoryTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        chronicle_name, story_name, start_time, end_time,
        event_data, buffer_size);
    return ipc_manager->Send(task);
  }

  chi::Future<ReplayChronicleTask> AsyncReplayChronicle(
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name,
      chi::u64 start_time,
      chi::u64 end_time) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<ReplayChronicleTask>(
        chi::CreateTaskId(), pool_id_, pool_query);
    return ipc_manager->Send(task);
  }

  chi::Future<GetStoryInfoTask> AsyncGetStoryInfo(
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name,
      const std::string& story_name) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<GetStoryInfoTask>(
        chi::CreateTaskId(), pool_id_, pool_query);
    return ipc_manager->Send(task);
  }
};

}  // namespace chronolog::player

#endif  // CHRONOLOG_PLAYER_CLIENT_H_
