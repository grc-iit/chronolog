#ifndef CHRONOLOG_KEEPER_TASKS_H_
#define CHRONOLOG_KEEPER_TASKS_H_

#include <chimaera/chimaera.h>
#include "autogen/keeper_methods.h"
#include <chimaera/admin/admin_tasks.h>

namespace chronolog::keeper {

using MonitorTask = chimaera::admin::MonitorTask;

struct CreateParams {
  chi::u32 max_story_chunk_size_;
  chi::u32 story_chunk_duration_secs_;
  chi::u32 acceptance_window_secs_;

  static constexpr const char* chimod_lib_name = "chronolog_keeper";

  CreateParams(chi::u32 max_story_chunk_size = 4096,
               chi::u32 story_chunk_duration_secs = 10,
               chi::u32 acceptance_window_secs = 15)
      : max_story_chunk_size_(max_story_chunk_size),
        story_chunk_duration_secs_(story_chunk_duration_secs),
        acceptance_window_secs_(acceptance_window_secs) {}

  template<class Archive>
  void serialize(Archive& ar) {
    ar(max_story_chunk_size_, story_chunk_duration_secs_, acceptance_window_secs_);
  }

#if HSHM_IS_HOST
  void LoadConfig(const chi::PoolConfig& pool_config) {
    (void)pool_config;
    // Keeper uses default parameters; compose mode not yet supported
  }
#endif
};

using CreateTask = chimaera::admin::GetOrCreatePoolTask<CreateParams>;
using DestroyTask = chimaera::admin::DestroyTask;

// =========================================================================
// RecordEventTask
// =========================================================================
struct RecordEventTask : public chi::Task {
  IN chi::u64 story_id_;
  IN chi::u64 event_time_;
  IN chi::u64 client_id_;
  IN chi::u32 event_index_;
  IN chi::priv::string log_record_;

  RecordEventTask()
      : chi::Task(),
        story_id_(0), event_time_(0), client_id_(0), event_index_(0),
        log_record_(CHI_PRIV_ALLOC) {}

  explicit RecordEventTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      chi::u64 event_time,
      chi::u64 client_id,
      chi::u32 event_index,
      const std::string& log_record)
      : chi::Task(task_node, pool_id, pool_query, Method::kRecordEvent),
        story_id_(story_id), event_time_(event_time),
        client_id_(client_id), event_index_(event_index),
        log_record_(CHI_PRIV_ALLOC, log_record) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kRecordEvent;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(story_id_, event_time_, client_id_, event_index_, log_record_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
  }

  void Copy(const hipc::FullPtr<RecordEventTask>& other) {
    Task::Copy(other.template Cast<Task>());
    story_id_ = other->story_id_;
    event_time_ = other->event_time_;
    client_id_ = other->client_id_;
    event_index_ = other->event_index_;
    log_record_ = other->log_record_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<RecordEventTask>());
  }
};

// =========================================================================
// AcquireStoryTask
// =========================================================================
struct AcquireStoryTask : public chi::Task {
  IN chi::priv::string chronicle_name_;
  IN chi::priv::string story_name_;
  OUT chi::u64 story_id_;

  AcquireStoryTask()
      : chi::Task(),
        chronicle_name_(CHI_PRIV_ALLOC),
        story_name_(CHI_PRIV_ALLOC),
        story_id_(0) {}

  explicit AcquireStoryTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name,
      const std::string& story_name)
      : chi::Task(task_node, pool_id, pool_query, Method::kAcquireStory),
        chronicle_name_(CHI_PRIV_ALLOC, chronicle_name),
        story_name_(CHI_PRIV_ALLOC, story_name),
        story_id_(0) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kAcquireStory;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(chronicle_name_, story_name_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(story_id_);
  }

  void Copy(const hipc::FullPtr<AcquireStoryTask>& other) {
    Task::Copy(other.template Cast<Task>());
    chronicle_name_ = other->chronicle_name_;
    story_name_ = other->story_name_;
    story_id_ = other->story_id_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<AcquireStoryTask>());
  }
};

// =========================================================================
// ReleaseStoryTask
// =========================================================================
struct ReleaseStoryTask : public chi::Task {
  IN chi::u64 story_id_;
  IN chi::u64 client_id_;

  ReleaseStoryTask()
      : chi::Task(), story_id_(0), client_id_(0) {}

  explicit ReleaseStoryTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      chi::u64 client_id)
      : chi::Task(task_node, pool_id, pool_query, Method::kReleaseStory),
        story_id_(story_id), client_id_(client_id) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kReleaseStory;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(story_id_, client_id_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
  }

  void Copy(const hipc::FullPtr<ReleaseStoryTask>& other) {
    Task::Copy(other.template Cast<Task>());
    story_id_ = other->story_id_;
    client_id_ = other->client_id_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<ReleaseStoryTask>());
  }
};

// =========================================================================
// CreateChronicleTask
// =========================================================================
struct CreateChronicleTask : public chi::Task {
  IN chi::priv::string chronicle_name_;

  CreateChronicleTask()
      : chi::Task(),
        chronicle_name_(CHI_PRIV_ALLOC) {}

  explicit CreateChronicleTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name)
      : chi::Task(task_node, pool_id, pool_query, Method::kCreateChronicle),
        chronicle_name_(CHI_PRIV_ALLOC, chronicle_name) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kCreateChronicle;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(chronicle_name_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
  }

  void Copy(const hipc::FullPtr<CreateChronicleTask>& other) {
    Task::Copy(other.template Cast<Task>());
    chronicle_name_ = other->chronicle_name_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<CreateChronicleTask>());
  }
};

// =========================================================================
// Stub tasks: RecordEventBatch, StartStoryRecording, StopStoryRecording,
//             DestroyChronicle, ShowChronicles, ShowStories
// =========================================================================

struct RecordEventBatchTask : public chi::Task {
  IN chi::u32 event_count_;

  RecordEventBatchTask() : chi::Task(), event_count_(0) {}
  explicit RecordEventBatchTask(const chi::TaskId& task_id, const chi::PoolId& pool_id,
                       const chi::PoolQuery& pool_query,
                       chi::u32 event_count)
      : chi::Task(task_id, pool_id, pool_query, Method::kRecordEventBatch),
        event_count_(event_count) {}

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(event_count_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<RecordEventBatchTask>& other) {
    Task::Copy(other.template Cast<Task>());
    event_count_ = other->event_count_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<RecordEventBatchTask>());
  }
};

struct StartStoryRecordingTask : public chi::Task {
  IN chi::u64 story_id_;
  IN chi::priv::string chronicle_name_;
  IN chi::priv::string story_name_;
  IN chi::u64 start_time_;

  StartStoryRecordingTask()
      : chi::Task(), story_id_(0),
        chronicle_name_(CHI_PRIV_ALLOC),
        story_name_(CHI_PRIV_ALLOC),
        start_time_(0) {}

  explicit StartStoryRecordingTask(
      const chi::TaskId& task_id, const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      const std::string& chronicle_name,
      const std::string& story_name,
      chi::u64 start_time)
      : chi::Task(task_id, pool_id, pool_query, Method::kStartStoryRecording),
        story_id_(story_id),
        chronicle_name_(CHI_PRIV_ALLOC, chronicle_name),
        story_name_(CHI_PRIV_ALLOC, story_name),
        start_time_(start_time) {
    task_id_ = task_id;
    pool_id_ = pool_id;
    method_ = Method::kStartStoryRecording;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(story_id_, chronicle_name_, story_name_, start_time_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<StartStoryRecordingTask>& other) {
    Task::Copy(other.template Cast<Task>());
    story_id_ = other->story_id_;
    chronicle_name_ = other->chronicle_name_;
    story_name_ = other->story_name_;
    start_time_ = other->start_time_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<StartStoryRecordingTask>());
  }
};

struct StopStoryRecordingTask : public chi::Task {
  IN chi::u64 story_id_;

  StopStoryRecordingTask() : chi::Task(), story_id_(0) {}

  explicit StopStoryRecordingTask(
      const chi::TaskId& task_id, const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      chi::u64 story_id)
      : chi::Task(task_id, pool_id, pool_query, Method::kStopStoryRecording),
        story_id_(story_id) {
    task_id_ = task_id;
    pool_id_ = pool_id;
    method_ = Method::kStopStoryRecording;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(story_id_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<StopStoryRecordingTask>& other) {
    Task::Copy(other.template Cast<Task>());
    story_id_ = other->story_id_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<StopStoryRecordingTask>());
  }
};

struct DestroyChronicleTask : public chi::Task {
  IN chi::priv::string chronicle_name_;

  DestroyChronicleTask()
      : chi::Task(), chronicle_name_(CHI_PRIV_ALLOC) {}

  explicit DestroyChronicleTask(
      const chi::TaskId& task_id, const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name)
      : chi::Task(task_id, pool_id, pool_query, Method::kDestroyChronicle),
        chronicle_name_(CHI_PRIV_ALLOC, chronicle_name) {
    task_id_ = task_id;
    pool_id_ = pool_id;
    method_ = Method::kDestroyChronicle;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(chronicle_name_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) { Task::SerializeOut(ar); }

  void Copy(const hipc::FullPtr<DestroyChronicleTask>& other) {
    Task::Copy(other.template Cast<Task>());
    chronicle_name_ = other->chronicle_name_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<DestroyChronicleTask>());
  }
};

struct ShowChroniclesTask : public chi::Task {
  OUT chi::u32 count_;

  ShowChroniclesTask() : chi::Task(), count_(0) {}
  explicit ShowChroniclesTask(const chi::TaskId& task_id, const chi::PoolId& pool_id,
                       const chi::PoolQuery& pool_query)
      : chi::Task(task_id, pool_id, pool_query, Method::kShowChronicles),
        count_(0) {
    task_id_ = task_id;
    pool_id_ = pool_id;
    method_ = Method::kShowChronicles;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) { Task::SerializeIn(ar); }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(count_);
  }

  void Copy(const hipc::FullPtr<ShowChroniclesTask>& other) {
    Task::Copy(other.template Cast<Task>());
    count_ = other->count_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<ShowChroniclesTask>());
  }
};

struct ShowStoriesTask : public chi::Task {
  IN chi::priv::string chronicle_name_;
  OUT chi::u32 count_;

  ShowStoriesTask()
      : chi::Task(), chronicle_name_(CHI_PRIV_ALLOC), count_(0) {}

  explicit ShowStoriesTask(
      const chi::TaskId& task_id, const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      const std::string& chronicle_name)
      : chi::Task(task_id, pool_id, pool_query, Method::kShowStories),
        chronicle_name_(CHI_PRIV_ALLOC, chronicle_name),
        count_(0) {
    task_id_ = task_id;
    pool_id_ = pool_id;
    method_ = Method::kShowStories;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(chronicle_name_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(count_);
  }

  void Copy(const hipc::FullPtr<ShowStoriesTask>& other) {
    Task::Copy(other.template Cast<Task>());
    chronicle_name_ = other->chronicle_name_;
    count_ = other->count_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<ShowStoriesTask>());
  }
};

}  // namespace chronolog::keeper

#endif  // CHRONOLOG_KEEPER_TASKS_H_
