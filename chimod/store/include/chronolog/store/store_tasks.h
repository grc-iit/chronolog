#ifndef CHRONOLOG_STORE_TASKS_H_
#define CHRONOLOG_STORE_TASKS_H_

#include <chimaera/chimaera.h>
#include "autogen/store_methods.h"
#include <chimaera/admin/admin_tasks.h>

namespace chronolog::store {

using MonitorTask = chimaera::admin::MonitorTask;

struct CreateParams {
  std::string archive_root_;

  static constexpr const char* chimod_lib_name = "chronolog_store";

  CreateParams(const std::string& archive_root = "/tmp/chronolog/archive")
      : archive_root_(archive_root) {}

  template<class Archive>
  void serialize(Archive& ar) {
    ar(archive_root_);
  }

#if HSHM_IS_HOST
  void LoadConfig(const chi::PoolConfig& pool_config) {
    (void)pool_config;
  }
#endif
};

using CreateTask = chimaera::admin::GetOrCreatePoolTask<CreateParams>;
using DestroyTask = chimaera::admin::DestroyTask;

// =========================================================================
// ArchiveChunkTask
// =========================================================================
struct ArchiveChunkTask : public chi::Task {
  IN chi::u64 story_id_;
  IN chi::priv::string chronicle_name_;
  IN chi::priv::string story_name_;
  IN chi::u64 start_time_;
  IN chi::u64 end_time_;

  ArchiveChunkTask()
      : chi::Task(),
        story_id_(0),
        chronicle_name_(CHI_PRIV_ALLOC),
        story_name_(CHI_PRIV_ALLOC),
        start_time_(0), end_time_(0) {}

  explicit ArchiveChunkTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      const std::string& chronicle_name,
      const std::string& story_name,
      chi::u64 start_time,
      chi::u64 end_time)
      : chi::Task(task_node, pool_id, pool_query, Method::kArchiveChunk),
        story_id_(story_id),
        chronicle_name_(CHI_PRIV_ALLOC, chronicle_name),
        story_name_(CHI_PRIV_ALLOC, story_name),
        start_time_(start_time), end_time_(end_time) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kArchiveChunk;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(story_id_, chronicle_name_, story_name_, start_time_, end_time_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
  }

  void Copy(const hipc::FullPtr<ArchiveChunkTask>& other) {
    Task::Copy(other.template Cast<Task>());
    story_id_ = other->story_id_;
    chronicle_name_ = other->chronicle_name_;
    story_name_ = other->story_name_;
    start_time_ = other->start_time_;
    end_time_ = other->end_time_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<ArchiveChunkTask>());
  }
};

// =========================================================================
// ReadChunkTask
// =========================================================================
struct ReadChunkTask : public chi::Task {
  IN chi::priv::string chronicle_name_;
  IN chi::priv::string story_name_;
  IN chi::u64 start_time_;
  IN chi::u64 end_time_;
  OUT chi::u32 event_count_;

  ReadChunkTask()
      : chi::Task(),
        chronicle_name_(CHI_PRIV_ALLOC),
        story_name_(CHI_PRIV_ALLOC),
        start_time_(0), end_time_(0), event_count_(0) {}

  explicit ReadChunkTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name,
      const std::string& story_name,
      chi::u64 start_time,
      chi::u64 end_time)
      : chi::Task(task_node, pool_id, pool_query, Method::kReadChunk),
        chronicle_name_(CHI_PRIV_ALLOC, chronicle_name),
        story_name_(CHI_PRIV_ALLOC, story_name),
        start_time_(start_time), end_time_(end_time), event_count_(0) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kReadChunk;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(chronicle_name_, story_name_, start_time_, end_time_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(event_count_);
  }

  void Copy(const hipc::FullPtr<ReadChunkTask>& other) {
    Task::Copy(other.template Cast<Task>());
    chronicle_name_ = other->chronicle_name_;
    story_name_ = other->story_name_;
    start_time_ = other->start_time_;
    end_time_ = other->end_time_;
    event_count_ = other->event_count_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<ReadChunkTask>());
  }
};

// =========================================================================
// Stub tasks: ListArchives, DeleteArchive, CompactArchive
// =========================================================================

struct ListArchivesTask : public chi::Task {
  ListArchivesTask() : chi::Task() {}
  explicit ListArchivesTask(const chi::TaskId& task_id, const chi::PoolId& pool_id,
                            const chi::PoolQuery& pool_query)
      : chi::Task(task_id, pool_id, pool_query, Method::kListArchives) {}

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) { Task::SerializeIn(ar); }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<ListArchivesTask>& other) {
    Task::Copy(other.template Cast<Task>());
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
  }
};

struct DeleteArchiveTask : public chi::Task {
  DeleteArchiveTask() : chi::Task() {}
  explicit DeleteArchiveTask(const chi::TaskId& task_id, const chi::PoolId& pool_id,
                             const chi::PoolQuery& pool_query)
      : chi::Task(task_id, pool_id, pool_query, Method::kDeleteArchive) {}

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) { Task::SerializeIn(ar); }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<DeleteArchiveTask>& other) {
    Task::Copy(other.template Cast<Task>());
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
  }
};

struct CompactArchiveTask : public chi::Task {
  CompactArchiveTask() : chi::Task() {}
  explicit CompactArchiveTask(const chi::TaskId& task_id, const chi::PoolId& pool_id,
                              const chi::PoolQuery& pool_query)
      : chi::Task(task_id, pool_id, pool_query, Method::kCompactArchive) {}

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) { Task::SerializeIn(ar); }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<CompactArchiveTask>& other) {
    Task::Copy(other.template Cast<Task>());
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
  }
};

}  // namespace chronolog::store

#endif  // CHRONOLOG_STORE_TASKS_H_
