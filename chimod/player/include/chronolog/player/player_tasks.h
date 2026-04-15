#ifndef CHRONOLOG_PLAYER_TASKS_H_
#define CHRONOLOG_PLAYER_TASKS_H_

#include <chimaera/chimaera.h>
#include "autogen/player_methods.h"
#include <chimaera/admin/admin_tasks.h>

namespace chronolog::player {

using MonitorTask = chimaera::admin::MonitorTask;

struct CreateParams {
  std::string archive_root_;

  static constexpr const char* chimod_lib_name = "chronolog_player";

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
// ReplayStoryTask
// =========================================================================
struct ReplayStoryTask : public chi::Task {
  IN chi::priv::string chronicle_name_;
  IN chi::priv::string story_name_;
  IN chi::u64 start_time_;
  IN chi::u64 end_time_;
  OUT chi::u32 event_count_;

  ReplayStoryTask()
      : chi::Task(),
        chronicle_name_(CHI_PRIV_ALLOC),
        story_name_(CHI_PRIV_ALLOC),
        start_time_(0), end_time_(0), event_count_(0) {}

  explicit ReplayStoryTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name,
      const std::string& story_name,
      chi::u64 start_time,
      chi::u64 end_time)
      : chi::Task(task_node, pool_id, pool_query, Method::kReplayStory),
        chronicle_name_(CHI_PRIV_ALLOC, chronicle_name),
        story_name_(CHI_PRIV_ALLOC, story_name),
        start_time_(start_time), end_time_(end_time), event_count_(0) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kReplayStory;
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

  void Copy(const hipc::FullPtr<ReplayStoryTask>& other) {
    Task::Copy(other.template Cast<Task>());
    chronicle_name_ = other->chronicle_name_;
    story_name_ = other->story_name_;
    start_time_ = other->start_time_;
    end_time_ = other->end_time_;
    event_count_ = other->event_count_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<ReplayStoryTask>());
  }
};

// =========================================================================
// ReplayChronicleTask (stub)
// =========================================================================
struct ReplayChronicleTask : public chi::Task {
  IN chi::priv::string chronicle_name_;
  IN chi::u64 start_time_;
  IN chi::u64 end_time_;

  ReplayChronicleTask()
      : chi::Task(),
        chronicle_name_(CHI_PRIV_ALLOC),
        start_time_(0), end_time_(0) {}

  explicit ReplayChronicleTask(const chi::TaskId& task_id, const chi::PoolId& pool_id,
                               const chi::PoolQuery& pool_query)
      : chi::Task(task_id, pool_id, pool_query, Method::kReplayChronicle),
        chronicle_name_(CHI_PRIV_ALLOC), start_time_(0), end_time_(0) {}

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) { Task::SerializeIn(ar); }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<ReplayChronicleTask>& other) {
    Task::Copy(other.template Cast<Task>());
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
  }
};

// =========================================================================
// GetStoryInfoTask (stub)
// =========================================================================
struct GetStoryInfoTask : public chi::Task {
  IN chi::priv::string chronicle_name_;
  IN chi::priv::string story_name_;
  OUT chi::u64 earliest_time_;
  OUT chi::u64 latest_time_;

  GetStoryInfoTask()
      : chi::Task(),
        chronicle_name_(CHI_PRIV_ALLOC),
        story_name_(CHI_PRIV_ALLOC),
        earliest_time_(0), latest_time_(0) {}

  explicit GetStoryInfoTask(const chi::TaskId& task_id, const chi::PoolId& pool_id,
                            const chi::PoolQuery& pool_query)
      : chi::Task(task_id, pool_id, pool_query, Method::kGetStoryInfo),
        chronicle_name_(CHI_PRIV_ALLOC), story_name_(CHI_PRIV_ALLOC),
        earliest_time_(0), latest_time_(0) {}

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) { Task::SerializeIn(ar); }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<GetStoryInfoTask>& other) {
    Task::Copy(other.template Cast<Task>());
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
  }
};

}  // namespace chronolog::player

#endif  // CHRONOLOG_PLAYER_TASKS_H_
