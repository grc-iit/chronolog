#include "CteHelper.h"

#include <cstring>

#include <chimaera/chimaera.h>
#include <wrp_cte/core/core_client.h>

#include "StoryChunkSerializer.h"

namespace chronolog {

CteHelper::CteHelper() : cte_(WRP_CTE_CLIENT) {}

wrp_cte::core::TagId CteHelper::getOrCreateTag(
    const std::string& chronicle_name) {
  auto future = cte_->AsyncGetOrCreateTag(chronicle_name);
  future.Wait();
  return future->tag_id_;
}

bool CteHelper::putChunk(const wrp_cte::core::TagId& tag_id,
                         const StoryChunk& chunk, float score) {
  // Compute serialized size and allocate SHM buffer
  size_t blob_size = StoryChunkSerializer::computeSerializedSize(chunk);
  if (blob_size == 0) return true;  // empty chunk, nothing to store

  hipc::FullPtr<char> buf = CHI_IPC->AllocateBuffer(blob_size);
  if (buf.IsNull()) return false;

  // Serialize chunk into the buffer
  size_t written = StoryChunkSerializer::serialize(chunk, buf.ptr_, blob_size);
  if (written == 0) {
    CHI_IPC->FreeBuffer(buf);
    return false;
  }

  // Build blob name
  std::string blob_name = StoryChunkSerializer::makeBlobName(
      chunk.getStoryName(), chunk.getStartTime(), chunk.getEndTime());

  // Put blob via CTE
  hipc::ShmPtr<> shm_ptr = buf.shm_.template Cast<void>();
  auto future = cte_->AsyncPutBlob(
      tag_id, blob_name.c_str(), 0, written, shm_ptr, score,
      wrp_cte::core::Context(), 0);
  future.Wait();

  CHI_IPC->FreeBuffer(buf);
  return future->GetReturnCode() == 0;
}

StoryChunk* CteHelper::getChunk(const wrp_cte::core::TagId& tag_id,
                                const std::string& blob_name) {
  // First get blob size
  uint64_t blob_size = getBlobSize(tag_id, blob_name);
  if (blob_size == 0) return nullptr;

  // Allocate output buffer
  hipc::FullPtr<char> buf = CHI_IPC->AllocateBuffer(blob_size);
  if (buf.IsNull()) return nullptr;

  hipc::ShmPtr<> shm_ptr = buf.shm_.template Cast<void>();

  // Get blob from CTE
  auto future =
      cte_->AsyncGetBlob(tag_id, blob_name.c_str(), 0, blob_size, 0, shm_ptr);
  future.Wait();

  if (future->GetReturnCode() != 0) {
    CHI_IPC->FreeBuffer(buf);
    return nullptr;
  }

  // Deserialize
  StoryChunk* chunk =
      StoryChunkSerializer::deserialize(buf.ptr_, blob_size);

  CHI_IPC->FreeBuffer(buf);
  return chunk;
}

std::vector<std::string> CteHelper::listChunks(
    const wrp_cte::core::TagId& tag_id) {
  auto future = cte_->AsyncGetContainedBlobs(tag_id);
  future.Wait();

  if (future->GetReturnCode() != 0) return {};
  return future->blob_names_;
}

std::vector<std::string> CteHelper::queryChunks(
    const std::string& tag_regex, const std::string& blob_regex) {
  auto future = cte_->AsyncBlobQuery(tag_regex, blob_regex);
  future.Wait();

  if (future->GetReturnCode() != 0) return {};
  return future->blob_names_;
}

uint64_t CteHelper::getBlobSize(const wrp_cte::core::TagId& tag_id,
                                const std::string& blob_name) {
  auto future = cte_->AsyncGetBlobSize(tag_id, blob_name);
  future.Wait();

  if (future->GetReturnCode() != 0) return 0;
  return future->size_;
}

}  // namespace chronolog
