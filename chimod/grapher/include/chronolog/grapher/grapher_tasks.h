#ifndef CHRONOLOG_GRAPHER_TASKS_H_
#define CHRONOLOG_GRAPHER_TASKS_H_

#include <chimaera/chimaera.h>
#include "autogen/grapher_methods.h"
#include <chimaera/admin/admin_tasks.h>

namespace chronolog::grapher {

using MonitorTask = chimaera::admin::MonitorTask;

struct CreateParams {
  chi::u32 merge_timeout_secs_;
  chi::u32 archive_age_threshold_secs_;

  static constexpr const char* chimod_lib_name = "chronolog_grapher";

  CreateParams(chi::u32 merge_timeout_secs = 30,
               chi::u32 archive_age_threshold_secs = 300)
      : merge_timeout_secs_(merge_timeout_secs),
        archive_age_threshold_secs_(archive_age_threshold_secs) {}

  template<class Archive>
  void serialize(Archive& ar) {
    ar(merge_timeout_secs_, archive_age_threshold_secs_);
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
// MergeChunksTask
// =========================================================================
struct MergeChunksTask : public chi::Task {
  IN chi::u64 story_id_;
  IN chi::u64 window_start_;
  IN chi::u64 window_end_;
  OUT chi::u64 merged_blob_id_;

  MergeChunksTask()
      : chi::Task(),
        story_id_(0), window_start_(0), window_end_(0),
        merged_blob_id_(0) {}

  explicit MergeChunksTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      chi::u64 story_id,
      chi::u64 window_start,
      chi::u64 window_end)
      : chi::Task(task_node, pool_id, pool_query, Method::kMergeChunks),
        story_id_(story_id), window_start_(window_start),
        window_end_(window_end), merged_blob_id_(0) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kMergeChunks;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(story_id_, window_start_, window_end_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(merged_blob_id_);
  }

  void Copy(const hipc::FullPtr<MergeChunksTask>& other) {
    Task::Copy(other.template Cast<Task>());
    story_id_ = other->story_id_;
    window_start_ = other->window_start_;
    window_end_ = other->window_end_;
    merged_blob_id_ = other->merged_blob_id_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<MergeChunksTask>());
  }
};

// =========================================================================
// FlushStoryTask
// =========================================================================
struct FlushStoryTask : public chi::Task {
  IN chi::u64 story_id_;

  FlushStoryTask()
      : chi::Task(), story_id_(0) {}

  explicit FlushStoryTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      chi::u64 story_id)
      : chi::Task(task_node, pool_id, pool_query, Method::kFlushStory),
        story_id_(story_id) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kFlushStory;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(story_id_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
  }

  void Copy(const hipc::FullPtr<FlushStoryTask>& other) {
    Task::Copy(other.template Cast<Task>());
    story_id_ = other->story_id_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<FlushStoryTask>());
  }
};

// =========================================================================
// GetMergeStatusTask
// =========================================================================
struct GetMergeStatusTask : public chi::Task {
  IN chi::u64 story_id_;
  OUT chi::u32 pending_merges_;

  GetMergeStatusTask()
      : chi::Task(), story_id_(0), pending_merges_(0) {}

  explicit GetMergeStatusTask(
      const chi::TaskId& task_node,
      const chi::PoolId& pool_id,
      const chi::PoolQuery& pool_query,
      chi::u64 story_id)
      : chi::Task(task_node, pool_id, pool_query, Method::kGetMergeStatus),
        story_id_(story_id), pending_merges_(0) {
    task_id_ = task_node;
    pool_id_ = pool_id;
    method_ = Method::kGetMergeStatus;
    task_flags_.Clear();
    pool_query_ = pool_query;
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeIn(Archive& ar) {
    Task::SerializeIn(ar);
    ar(story_id_);
  }

  template<typename Archive>
  HSHM_CROSS_FUN void SerializeOut(Archive& ar) {
    Task::SerializeOut(ar);
    ar(pending_merges_);
  }

  void Copy(const hipc::FullPtr<GetMergeStatusTask>& other) {
    Task::Copy(other.template Cast<Task>());
    story_id_ = other->story_id_;
    pending_merges_ = other->pending_merges_;
  }

  void Aggregate(const hipc::FullPtr<chi::Task>& other_base) {
    Task::Aggregate(other_base);
    Copy(other_base.template Cast<GetMergeStatusTask>());
  }
};

}  // namespace chronolog::grapher

#endif  // CHRONOLOG_GRAPHER_TASKS_H_
