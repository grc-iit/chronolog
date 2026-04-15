#include "chronolog_client.h"

#include <chrono>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>

#include <chimaera/chimaera.h>
#include <chronolog/keeper/keeper_client.h>
#include <chronolog/player/player_client.h>

namespace chronolog {

// =========================================================================
// StoryHandleImpl — wraps a story_id and records events via keeper
// =========================================================================
class StoryHandleImpl : public StoryHandle {
 public:
  StoryHandleImpl(chronolog::keeper::Client* keeper,
                  uint64_t story_id,
                  uint64_t client_id)
      : keeper_(keeper),
        story_id_(story_id),
        client_id_(client_id),
        event_index_(0) {}

  uint64_t log_event(std::string const& record) override {
    auto now = std::chrono::high_resolution_clock::now();
    uint64_t timestamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch())
            .count();
    uint32_t idx = event_index_.fetch_add(1);

    auto future = keeper_->AsyncRecordEvent(
        chi::PoolQuery::Local(),
        story_id_, timestamp, client_id_, idx, record);
    future.Wait();
    return timestamp;
  }

  uint64_t story_id() const { return story_id_; }

 private:
  chronolog::keeper::Client* keeper_;
  uint64_t story_id_;
  uint64_t client_id_;
  std::atomic<uint32_t> event_index_;
};

// =========================================================================
// ClientImpl — PIMPL holding chimod clients and handle cache
// =========================================================================
class ClientImpl {
 public:
  chronolog::keeper::Client keeper_;
  chronolog::player::Client player_;
  uint64_t client_id_;

  // (chronicle, story) -> StoryHandleImpl*
  std::map<std::pair<std::string, std::string>, StoryHandleImpl*> handles_;
  std::mutex mutex_;

  ClientImpl(const chi::PoolId& keeper_pool_id,
             const chi::PoolId& player_pool_id)
      : keeper_(keeper_pool_id),
        player_(player_pool_id) {
    // Generate a unique client ID from address + time
    auto now = std::chrono::high_resolution_clock::now();
    client_id_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     now.time_since_epoch())
                     .count();
  }

  ~ClientImpl() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, handle] : handles_) {
      delete handle;
    }
    handles_.clear();
  }
};

// =========================================================================
// Client
// =========================================================================

Client::Client(const chi::PoolId& keeper_pool_id,
               const chi::PoolId& player_pool_id)
    : impl_(std::make_unique<ClientImpl>(keeper_pool_id, player_pool_id)) {}

Client::~Client() = default;

int Client::CreateChronicle(const std::string& chronicle_name) {
  auto future = impl_->keeper_.AsyncCreateChronicle(
      chi::PoolQuery::Local(), chronicle_name);
  future.Wait();
  return future->GetReturnCode();
}

int Client::DestroyChronicle(const std::string& chronicle_name) {
  auto future = impl_->keeper_.AsyncDestroyChronicle(
      chi::PoolQuery::Local(), chronicle_name);
  future.Wait();
  return future->GetReturnCode();
}

std::pair<int, StoryHandle*> Client::AcquireStory(
    const std::string& chronicle_name,
    const std::string& story_name) {
  auto future = impl_->keeper_.AsyncAcquireStory(
      chi::PoolQuery::Local(), chronicle_name, story_name);
  future.Wait();

  int rc = future->GetReturnCode();
  if (rc != CL_SUCCESS) {
    return {rc, nullptr};
  }

  uint64_t story_id = future->story_id_;
  auto* handle =
      new StoryHandleImpl(&impl_->keeper_, story_id, impl_->client_id_);

  std::lock_guard<std::mutex> lock(impl_->mutex_);
  auto key = std::make_pair(chronicle_name, story_name);
  // Clean up any existing handle for this key
  auto it = impl_->handles_.find(key);
  if (it != impl_->handles_.end()) {
    delete it->second;
  }
  impl_->handles_[key] = handle;

  return {CL_SUCCESS, handle};
}

int Client::ReleaseStory(const std::string& chronicle_name,
                         const std::string& story_name) {
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  auto key = std::make_pair(chronicle_name, story_name);
  auto it = impl_->handles_.find(key);
  if (it == impl_->handles_.end()) {
    return CL_ERR_NOT_ACQUIRED;
  }

  uint64_t story_id = it->second->story_id();
  delete it->second;
  impl_->handles_.erase(it);

  auto future = impl_->keeper_.AsyncReleaseStory(
      chi::PoolQuery::Local(), story_id, impl_->client_id_);
  future.Wait();
  return future->GetReturnCode();
}

int Client::ReplayStory(const std::string& chronicle_name,
                        const std::string& story_name,
                        uint64_t start,
                        uint64_t end,
                        std::vector<Event>& events) {
  // Allocate a SHM buffer for receiving events
  constexpr uint64_t kDefaultBufSize = 4 * 1024 * 1024;  // 4 MB
  auto* ipc_manager = CHI_IPC;
  hipc::FullPtr<char> buf = ipc_manager->AllocateBuffer(kDefaultBufSize);
  if (buf.IsNull()) {
    return CL_ERR_UNKNOWN;
  }
  hipc::ShmPtr<> shm_ptr = buf.shm_.template Cast<void>();

  auto future = impl_->player_.AsyncReplayStory(
      chi::PoolQuery::Local(),
      chronicle_name, story_name, start, end,
      shm_ptr, kDefaultBufSize);
  future.Wait();

  int rc = future->GetReturnCode();
  if (rc != 0) {
    ipc_manager->FreeBuffer(buf);
    return rc;
  }

  // Deserialize events from the packed buffer
  // Format: [u64 time][u64 client_id][u32 index][u32 record_len][char[] record]
  uint32_t event_count = future->event_count_;
  uint64_t bytes_written = future->bytes_written_;
  char* data = buf.ptr_;
  uint64_t offset = 0;

  events.reserve(events.size() + event_count);
  for (uint32_t i = 0; i < event_count && offset < bytes_written; ++i) {
    if (offset + sizeof(uint64_t) * 2 + sizeof(uint32_t) * 2 > bytes_written) {
      break;
    }

    uint64_t ev_time;
    uint64_t ev_client_id;
    uint32_t ev_index;
    uint32_t record_len;

    memcpy(&ev_time, data + offset, sizeof(ev_time));
    offset += sizeof(ev_time);
    memcpy(&ev_client_id, data + offset, sizeof(ev_client_id));
    offset += sizeof(ev_client_id);
    memcpy(&ev_index, data + offset, sizeof(ev_index));
    offset += sizeof(ev_index);
    memcpy(&record_len, data + offset, sizeof(record_len));
    offset += sizeof(record_len);

    if (offset + record_len > bytes_written) {
      break;
    }

    std::string record(data + offset, record_len);
    offset += record_len;

    events.emplace_back(ev_time, ev_client_id, ev_index, record);
  }

  ipc_manager->FreeBuffer(buf);
  return CL_SUCCESS;
}

int Client::ShowChronicles(std::vector<std::string>& out) {
  auto future = impl_->keeper_.AsyncShowChronicles(
      chi::PoolQuery::Local());
  future.Wait();
  // ShowChroniclesTask only returns count_ — names not yet supported
  (void)out;
  return future->GetReturnCode();
}

int Client::ShowStories(const std::string& chronicle_name,
                        std::vector<std::string>& out) {
  auto future = impl_->keeper_.AsyncShowStories(
      chi::PoolQuery::Local(), chronicle_name);
  future.Wait();
  // ShowStoriesTask only returns count_ — names not yet supported
  (void)out;
  return future->GetReturnCode();
}

}  // namespace chronolog
