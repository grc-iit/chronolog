#ifndef CHRONOLOG_GRAPHER_CLIENT_H_
#define CHRONOLOG_GRAPHER_CLIENT_H_

#include <chimaera/chimaera.h>
#include "grapher_tasks.h"

namespace chronolog::grapher {

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

  chi::Future<MergeChunksTask> AsyncMergeChunks(
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      chi::u64 window_start,
      chi::u64 window_end) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<MergeChunksTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        story_id, window_start, window_end);
    return ipc_manager->Send(task);
  }

  chi::Future<FlushStoryTask> AsyncFlushStory(
      const chi::PoolQuery& pool_query,
      chi::u64 story_id) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<FlushStoryTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        story_id);
    return ipc_manager->Send(task);
  }

  chi::Future<GetMergeStatusTask> AsyncGetMergeStatus(
      const chi::PoolQuery& pool_query,
      chi::u64 story_id) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<GetMergeStatusTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        story_id);
    return ipc_manager->Send(task);
  }
};

}  // namespace chronolog::grapher

#endif  // CHRONOLOG_GRAPHER_CLIENT_H_
