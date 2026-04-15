#ifndef CHRONOLOG_STORE_CLIENT_H_
#define CHRONOLOG_STORE_CLIENT_H_

#include <chimaera/chimaera.h>
#include "store_tasks.h"

namespace chronolog::store {

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

  chi::Future<ArchiveChunkTask> AsyncArchiveChunk(
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      const std::string& chronicle_name,
      const std::string& story_name,
      chi::u64 start_time,
      chi::u64 end_time) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<ArchiveChunkTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        story_id, chronicle_name, story_name, start_time, end_time);
    return ipc_manager->Send(task);
  }

  chi::Future<ReadChunkTask> AsyncReadChunk(
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name,
      const std::string& story_name,
      chi::u64 start_time,
      chi::u64 end_time) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<ReadChunkTask>(
        chi::CreateTaskId(), pool_id_, pool_query,
        chronicle_name, story_name, start_time, end_time);
    return ipc_manager->Send(task);
  }

  chi::Future<ListArchivesTask> AsyncListArchives(
      const chi::PoolQuery& pool_query) {
    auto* ipc_manager = CHI_IPC;
    auto task = ipc_manager->NewTask<ListArchivesTask>(
        chi::CreateTaskId(), pool_id_, pool_query);
    return ipc_manager->Send(task);
  }
};

}  // namespace chronolog::store

#endif  // CHRONOLOG_STORE_CLIENT_H_
