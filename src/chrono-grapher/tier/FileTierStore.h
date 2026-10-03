#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <set>
#include <span>
#include "chrono-grapher/tier/ChunkCodec.h"
#include "chrono-grapher/tier/ManifestLog.h"
#include "chronolog/tier_store.h"

namespace chronolog
{
class ArchiveReaderPool;
class FileTierStore final: public TierStore
{
public:
    using Unlink = std::function<int(const std::filesystem::path&)>;
    using LoadFile = std::function<absl::StatusOr<ChunkBytes>(const std::filesystem::path&)>;
    using DecodeFile = std::function<absl::StatusOr<std::vector<Event>>(const std::filesystem::path&, ChunkBytes&)>;
    ~FileTierStore() override;
    static absl::StatusOr<std::unique_ptr<FileTierStore>>
    Open(std::filesystem::path root,
         std::string manifest_writer,
         std::map<StoryId, Hlc> anchors = {},
         std::shared_ptr<const ChunkCodec> codec = std::make_shared<HDF5ChunkCodec>(),
         Unlink unlink = {},
         LoadFile load_file = {},
         size_t read_threads = 0,
         DecodeFile decode_file = {});
    static absl::StatusOr<std::unique_ptr<FileTierStore>>
    OpenReadOnly(std::filesystem::path root,
                 std::chrono::milliseconds manifest_poll = std::chrono::milliseconds(1000),
                 LoadFile load_file = {},
                 size_t read_threads = 0,
                 DecodeFile decode_file = {});
    absl::Status refreshNow() const;
    absl::Status registerStory(StoryId story, std::optional<Hlc> anchor = std::nullopt);
    absl::StatusOr<ManifestRecord> publish(Chunk chunk) override;
    absl::StatusOr<std::vector<Event>> read(StoryId story, Range range) const override;
    absl::StatusOr<std::vector<Event>>
    readRecord(const ManifestRecord& record, Range range, size_t max_events = SIZE_MAX) const;
    size_t readConcurrency() const { return read_threads_; }
    std::vector<absl::StatusOr<std::vector<Event>>>
    readRecords(std::span<const ManifestRecord> records, Range range, size_t max_events = SIZE_MAX) const;
    absl::StatusOr<std::vector<ManifestRecord>> manifest(StoryId story) const override;
    absl::StatusOr<Hlc> contiguousWatermark(StoryId story) const override;
    absl::StatusOr<bool> incomplete(StoryId story, Range range) const;
    absl::Status eraseFile(const std::string& file);
    absl::Status retryDeletedFiles();
    absl::StatusOr<bool> hasPendingUnlinks(StoryId story);
    // Appends the Tombstoned record and fsyncs it; publish refuses the story from then on, across restarts (I13.11).
    // Idempotent. A tombstone does not touch the story's files or its watermark.
    absl::Status tombstone(StoryId story);
    absl::StatusOr<bool> tombstoned(StoryId story) const;
    absl::StatusOr<std::vector<StoryId>> tombstonedStories() const;
    // Stories the manifest holds records for and no tombstone covers.
    absl::StatusOr<std::vector<StoryId>> liveStories() const;
    absl::Status compact();
    absl::StatusOr<std::vector<StoryId>> storiesWithoutPhysicalPolicy() const;

private:
    FileTierStore(std::filesystem::path root,
                  std::string writer,
                  std::unique_ptr<ManifestLog> log,
                  std::map<StoryId, Hlc> anchors,
                  std::shared_ptr<const ChunkCodec> codec,
                  Unlink unlink,
                  LoadFile load_file,
                  size_t read_threads,
                  DecodeFile decode_file);
    absl::Status recover();
    absl::StatusOr<std::vector<Event>> validate(const ManifestRecord& record) const;
    bool retired(const ManifestIndex& index, const ManifestRecord& record) const;
    absl::StatusOr<std::vector<Event>> afterVanished(const ManifestRecord& record, absl::Status failure) const;
    absl::StatusOr<bool> canReadRecord(const ManifestRecord& record, Range range) const;
    absl::StatusOr<std::vector<Event>>
    decodeRecord(const ManifestRecord& record, Range range, size_t max_events, ChunkBytes bytes) const;
    void collectDeletedFiles(const ManifestIndex& index);
    absl::Status unlinkDeletedFile(const std::string& file);
    struct StoryView
    {
        bool built{};
        uint64_t generation{};
        size_t applied{};
        std::vector<ManifestRecord> effective;
        std::vector<size_t> by_start;
        std::set<std::string> published;
        std::optional<Hlc> first_start;
        bool watermark_valid{};
        Hlc watermark_input, watermark;
    };
    absl::StatusOr<const ManifestIndex*> refresh() const;
    StoryView& viewOf(const ManifestIndex& index, StoryId story) const;
    std::vector<ManifestRecord> effective(const ManifestIndex& index, StoryId story) const;
    Hlc watermark(const ManifestIndex& index, StoryId story) const;
    bool known(const ManifestIndex& index, StoryId story) const;

    bool read_only_{};
    std::chrono::milliseconds manifest_poll_{1000};
    mutable bool polled_{};
    mutable std::map<StoryId, StoryView> views_;
    std::set<std::string> inflight_;
    std::condition_variable inflight_changed_;
    mutable std::chrono::steady_clock::time_point refreshed_{};
    // Forced refreshes after a vanished file: started_ counts those begun, done_ is the number of the last that
    // succeeded. A reader that saw a vanish reuses any refresh numbered above what started_ read at that moment.
    mutable std::atomic<uint64_t> forced_started_{};
    mutable uint64_t forced_done_{};
    std::filesystem::path root_;
    std::string writer_;
    std::unique_ptr<ManifestLog> log_;
    std::shared_ptr<const ChunkCodec> codec_;
    Unlink unlink_;
    LoadFile load_file_;
    DecodeFile decode_file_;
    const size_t read_threads_;
    std::map<std::string, StoryId> pending_unlinks_;
    uint64_t deletion_generation_{};
    size_t deletion_applied_{};
    mutable std::mutex mutex_;
    std::map<StoryId, std::optional<Hlc>> anchors_;
    mutable std::map<StoryId, Hlc> watermarks_;
    mutable std::unique_ptr<ArchiveReaderPool> readers_;
};
} // namespace chronolog
