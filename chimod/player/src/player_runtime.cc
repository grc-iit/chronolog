#include "chronolog/player/player_runtime.h"

#include <string>
#include <algorithm>
#include <cstring>

#include <CteHelper.h>
#include <StoryChunkSerializer.h>

namespace chronolog::player {

// =========================================================================
// Helper: pack one event into the SHM buffer
// Format: [u64 time][u64 client_id][u32 index][u32 record_len][char[] record]
// Returns bytes written, or 0 if buffer full.
// =========================================================================
static uint64_t packEvent(char* buf, uint64_t buf_size, uint64_t offset,
                          uint64_t ev_time, uint64_t client_id,
                          uint32_t ev_index, const std::string& record) {
  constexpr uint64_t kHeaderSize =
      sizeof(uint64_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
  uint32_t record_len = static_cast<uint32_t>(record.size());
  uint64_t needed = kHeaderSize + record_len;
  if (offset + needed > buf_size) return 0;

  memcpy(buf + offset, &ev_time, sizeof(ev_time));
  offset += sizeof(ev_time);
  memcpy(buf + offset, &client_id, sizeof(client_id));
  offset += sizeof(client_id);
  memcpy(buf + offset, &ev_index, sizeof(ev_index));
  offset += sizeof(ev_index);
  memcpy(buf + offset, &record_len, sizeof(record_len));
  offset += sizeof(record_len);
  memcpy(buf + offset, record.data(), record_len);
  return needed;
}

// =========================================================================
// Create - Initialize container
// =========================================================================
chi::TaskResume Runtime::Create(hipc::FullPtr<CreateTask> task,
                                chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  auto params = task->GetParams();
  archive_root_ = params.archive_root_;

  HLOG(kInfo, "player: Create pool {} (CTE-backed)", task->pool_id_);

  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Destroy - Clear all state
// =========================================================================
chi::TaskResume Runtime::Destroy(hipc::FullPtr<DestroyTask> task,
                                 chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  task->return_code_ = 0;
  task->error_message_ = "";
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// Monitor - No-op (CTE handles its own maintenance)
// =========================================================================
chi::TaskResume Runtime::Monitor(hipc::FullPtr<MonitorTask> task,
                                 chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ReplayStory - Query CTE for matching blobs and return events
// =========================================================================
chi::TaskResume Runtime::ReplayStory(hipc::FullPtr<ReplayStoryTask> task,
                                     chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());
  uint64_t start_time = task->start_time_;
  uint64_t end_time = task->end_time_;

  // Get the output buffer
  char* buf = nullptr;
  uint64_t buf_size = task->buffer_size_;
  if (!task->event_data_.IsNull() && buf_size > 0) {
    auto char_ptr = task->event_data_.template Cast<char>();
    hipc::FullPtr<char> buf_fullptr = CHI_IPC->ToFullPtr<char>(char_ptr);
    buf = buf_fullptr.ptr_;
  }

  uint32_t total_events = 0;
  uint64_t offset = 0;

  if (buf != nullptr) {
    // Get CTE tag for this chronicle
    auto tag_id = cte_helper_.getOrCreateTag(chronicle_name);
    auto blobs = cte_helper_.listChunks(tag_id);

    // Filter blobs by story name and time range overlap
    for (const auto& blob_name : blobs) {
      std::string parsed_story;
      uint64_t blob_start, blob_end;
      if (!chronolog::StoryChunkSerializer::parseBlobName(
              blob_name, parsed_story, blob_start, blob_end)) {
        continue;
      }
      if (parsed_story != story_name) continue;
      if (blob_start >= end_time || blob_end <= start_time) continue;

      // Fetch chunk from CTE
      chronolog::StoryChunk* chunk = cte_helper_.getChunk(tag_id, blob_name);
      if (!chunk) continue;

      // Pack matching events into the output buffer
      for (auto it = chunk->begin(); it != chunk->end(); ++it) {
        const chronolog::LogEvent& ev = it->second;
        if (ev.eventTime < start_time || ev.eventTime >= end_time) continue;

        uint64_t written = packEvent(
            buf, buf_size, offset,
            ev.eventTime, ev.clientId, ev.eventIndex, ev.logRecord);
        if (written == 0) {
          delete chunk;
          goto done;  // buffer full
        }
        offset += written;
        ++total_events;
      }
      delete chunk;
    }
  }

done:
  task->event_count_ = total_events;
  task->bytes_written_ = offset;
  HLOG(kDebug, "player: ReplayStory {}/{} [{}-{}] returned {} events ({} bytes)",
       chronicle_name, story_name, start_time, end_time,
       total_events, offset);

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// ReplayChronicle - Replay all stories in a chronicle
// =========================================================================
chi::TaskResume Runtime::ReplayChronicle(
    hipc::FullPtr<ReplayChronicleTask> task, chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  // TODO: Implement via CTE tag query
  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetStoryInfo - Find earliest and latest times for a story via CTE
// =========================================================================
chi::TaskResume Runtime::GetStoryInfo(hipc::FullPtr<GetStoryInfoTask> task,
                                      chi::RunContext& rctx) {
  CHI_TASK_BODY_BEGIN
  std::string chronicle_name(task->chronicle_name_.c_str());
  std::string story_name(task->story_name_.c_str());

  uint64_t earliest = UINT64_MAX;
  uint64_t latest = 0;

  auto tag_id = cte_helper_.getOrCreateTag(chronicle_name);
  auto blobs = cte_helper_.listChunks(tag_id);

  for (const auto& blob_name : blobs) {
    std::string parsed_story;
    uint64_t blob_start, blob_end;
    if (!chronolog::StoryChunkSerializer::parseBlobName(
            blob_name, parsed_story, blob_start, blob_end)) {
      continue;
    }
    if (parsed_story != story_name) continue;

    if (blob_start < earliest) earliest = blob_start;
    if (blob_end > latest) latest = blob_end;
  }

  if (earliest == UINT64_MAX) earliest = 0;

  task->earliest_time_ = earliest;
  task->latest_time_ = latest;

  HLOG(kDebug, "player: GetStoryInfo {}/{} earliest={} latest={}",
       chronicle_name, story_name, earliest, latest);

  task->SetReturnCode(0);
  (void)rctx;
  CHI_CO_RETURN;
  CHI_TASK_BODY_END
}

// =========================================================================
// GetWorkRemaining
// =========================================================================
chi::u64 Runtime::GetWorkRemaining() const {
  return 0;
}

}  // namespace chronolog::player
