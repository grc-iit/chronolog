#ifndef CHRONOLOG_CTE_HELPER_H
#define CHRONOLOG_CTE_HELPER_H

#include <string>
#include <vector>

#include <wrp_cte/core/core_client.h>

#include "StoryChunk.h"
#include "StoryChunkSerializer.h"

namespace chronolog {

/// Thin synchronous wrapper around CTE client operations.
/// Used by chimod runtimes to store/retrieve StoryChunks as CTE blobs.
class CteHelper {
 public:
  CteHelper();

  /// Get or create a CTE tag for the given chronicle name.
  wrp_cte::core::TagId getOrCreateTag(const std::string& chronicle_name);

  /// Serialize and store a StoryChunk as a CTE blob.
  /// Blob name: "{story}.{start_ns}.{end_ns}"
  /// Returns true on success.
  bool putChunk(const wrp_cte::core::TagId& tag_id, const StoryChunk& chunk,
                float score);

  /// Retrieve and deserialize a StoryChunk from CTE.
  /// Caller owns the returned pointer. Returns nullptr on failure.
  StoryChunk* getChunk(const wrp_cte::core::TagId& tag_id,
                       const std::string& blob_name);

  /// List all blob names in a tag.
  std::vector<std::string> listChunks(const wrp_cte::core::TagId& tag_id);

  /// Query blobs matching regex patterns. Returns matching blob names.
  std::vector<std::string> queryChunks(const std::string& tag_regex,
                                       const std::string& blob_regex);

  /// Get the size of a blob in bytes. Returns 0 on failure.
  uint64_t getBlobSize(const wrp_cte::core::TagId& tag_id,
                       const std::string& blob_name);

 private:
  wrp_cte::core::Client* cte_;
};

}  // namespace chronolog

#endif  // CHRONOLOG_CTE_HELPER_H
