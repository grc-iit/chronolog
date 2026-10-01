#pragma once

#include <chrono>
#include "chrono-grapher/tier/ChunkCodec.h"
#include "chrono-grapher/tier/ManifestLog.h"
#include "chronolog/tier_store.h"

namespace chronolog
{
class FileTierStore final: public TierStore
{
public:
    static absl::StatusOr<std::unique_ptr<FileTierStore>>
    Open(std::filesystem::path root,
         std::string manifest_writer,
         std::map<StoryId, Hlc> anchors = {},
         std::shared_ptr<const ChunkCodec> codec = std::make_shared<HDF5ChunkCodec>());
    static absl::StatusOr<std::unique_ptr<FileTierStore>>
    OpenReadOnly(std::filesystem::path root, std::chrono::milliseconds manifest_poll = std::chrono::milliseconds(1000));
    absl::Status refreshNow() const;
    absl::Status registerStory(StoryId story, std::optional<Hlc> anchor = std::nullopt);
    absl::StatusOr<ManifestRecord> publish(Chunk chunk) override;
    absl::StatusOr<std::vector<Event>> read(StoryId story, Range range) const override;
    absl::StatusOr<std::vector<ManifestRecord>> manifest(StoryId story) const override;
    absl::StatusOr<Hlc> contiguousWatermark(StoryId story) const override;
    absl::StatusOr<bool> incomplete(StoryId story, Range range) const;
    absl::Status eraseFile(const std::string& file);
    absl::Status compact();

private:
    FileTierStore(std::filesystem::path root,
                  std::string writer,
                  std::unique_ptr<ManifestLog> log,
                  std::map<StoryId, Hlc> anchors,
                  std::shared_ptr<const ChunkCodec> codec);
    absl::Status recover();
    absl::StatusOr<ManifestIndex> refresh() const;
    std::vector<ManifestRecord> effective(const ManifestIndex& index, StoryId story) const;
    Hlc watermark(const ManifestIndex& index, StoryId story) const;
    bool known(const ManifestIndex& index, StoryId story) const;

    bool read_only_{};
    std::chrono::milliseconds manifest_poll_{1000};
    mutable std::optional<ManifestIndex> cached_index_;
    mutable std::chrono::steady_clock::time_point refreshed_{};
    std::filesystem::path root_;
    std::string writer_;
    std::unique_ptr<ManifestLog> log_;
    std::shared_ptr<const ChunkCodec> codec_;
    mutable std::mutex mutex_;
    std::map<StoryId, std::optional<Hlc>> anchors_;
    mutable std::map<StoryId, Hlc> watermarks_;
};
} // namespace chronolog
