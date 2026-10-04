#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <set>
#include <span>
#include "tier/ChunkCodec.h"
#include "tier/ManifestLog.h"
#include "tier/PosixTier.h"
#include "chronolog/tier_store.h"

namespace chronolog
{
class ArchiveReaderPool;
// Bounds of one background compaction job (B3). Every input is an own, contiguous, non-exempt Published file at or
// below W; the job takes the longest such prefix inside every bound and needs at least min_files of them.
struct CompactionPolicy
{
    size_t min_files = 32;
    size_t max_files = 128;
    uint64_t small_file_bytes = 1024 * 1024;
    std::chrono::seconds min_age{300};
    int64_t max_span_ns = int64_t{3600} * 1000 * 1000 * 1000;
    uint64_t max_output_bytes = 32 * 1024 * 1024;
    // At most half the smallest Player read_max_events, so a read starting inside one output still progresses.
    uint64_t max_events = 65536;
    uint64_t io_bytes_per_sec = 4 * 1024 * 1024;
    uint64_t io_burst_bytes = 32 * 1024 * 1024;
};
struct CompactionResult
{
    size_t inputs{};
    std::string output;
};
class FileTierStore final: public TierStore
{
public:
    using Unlink = std::function<int(const std::filesystem::path&)>;
    using LoadFile = std::function<absl::StatusOr<ChunkBytes>(const std::filesystem::path&)>;
    using DecodeFile = std::function<absl::StatusOr<std::vector<Event>>(const std::filesystem::path&, ChunkBytes&)>;
    ~FileTierStore() override;
    // Fault injection. manifest_sync replaces fsync of this writer's manifest log; compaction_step runs at each named
    // compaction step ("link", "switch", "cleanup") and a non-OK result stops the job there as a crash would, leaving
    // every file in place.
    struct Hooks
    {
        std::function<int(int)> manifest_sync;
        std::function<absl::Status(int)> migration_step;
        std::function<absl::Status(std::string_view)> tier_step;
        std::function<absl::Status(std::string_view)> compaction_step;
    };
    static absl::StatusOr<std::unique_ptr<FileTierStore>>
    Open(std::filesystem::path root,
         std::string manifest_writer,
         std::map<StoryId, Hlc> anchors = {},
         std::shared_ptr<const ChunkCodec> codec = std::make_shared<HDF5ChunkCodec>(),
         Unlink unlink = {},
         LoadFile load_file = {},
         size_t read_threads = 0,
         DecodeFile decode_file = {},
         Hooks hooks = {},
         TierChain chain = {});
    static absl::StatusOr<std::unique_ptr<FileTierStore>>
    OpenReadOnly(std::filesystem::path root,
                 std::chrono::milliseconds manifest_poll = std::chrono::milliseconds(1000),
                 LoadFile load_file = {},
                 size_t read_threads = 0,
                 DecodeFile decode_file = {},
                 std::chrono::milliseconds archive_read_timeout = std::chrono::milliseconds(30000),
                 TierChain chain = {},
                 Hooks hooks = {});
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
    // Runs at most one compaction job on the calling worker: selects a run, writes and verifies the output, commits
    // the one switch line and unlinks the superseded inputs. inputs is zero when nothing was eligible. After any
    // manifest append or fsync failure it refuses until the writer reopens.
    absl::StatusOr<CompactionResult> compactOnce(const CompactionPolicy& policy);
    // Wakes and stops a job waiting for I/O budget or a publish, and every later job of this store, for shutdown.
    void stopCompaction();
    absl::Status configureTiers(std::string deployment,
                                std::vector<TierConfig> tiers,
                                size_t threads = 2,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    absl::Status probeTiers(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) const;
    absl::StatusOr<size_t> migrateOnce(const std::string& destination);
    absl::Status sweepTiers();
    absl::Status writeTierReplicas();
    absl::Status awaitTierUnlinksForTesting(std::chrono::milliseconds timeout);
    absl::StatusOr<ChunkBytes> loadResolvedFile(const std::string& file) const;
    absl::StatusOr<std::optional<MigrationLocation>> location(const std::string& file) const;


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
    absl::StatusOr<std::set<std::string>> recover();
    absl::StatusOr<std::vector<Event>> validate(const ManifestRecord& record,
                                                std::optional<FileChecksum> checksum = std::nullopt) const;
    bool retired(const ManifestIndex& index, const ManifestRecord& record) const;
    absl::StatusOr<std::vector<Event>> afterVanished(const ManifestRecord& record,
                                                     absl::Status failure,
                                                     Range range,
                                                     size_t max_events,
                                                     uint32_t planned_rank) const;
    std::optional<ManifestRecord> successor(const ManifestIndex& index, const ManifestRecord& record) const;
    absl::StatusOr<ChunkBytes> loadForRead(const std::filesystem::path& file) const;
    bool effectivePublished(const ManifestIndex& index, const ManifestRecord& record) const;
    absl::Status rollbackOrLose(ManifestRecord record, Hlc w);
    void queueCommittedCleanup(const std::set<std::string>& on_disk);
    void sweepTombstoned();
    struct Claim
    {
        StoryId story;
        std::string token;
    };
    std::shared_ptr<Claim> claim(const std::string& file, StoryId story);
    bool migrationEligible(const ManifestIndex& index,
                           const ManifestRecord& record,
                           const std::shared_ptr<Claim>& own = {}) const;
    absl::Status cleanupMigrations();
    void queueTierSweeps();
    std::map<std::pair<std::string, StoryId>, std::future<absl::Status>> tier_sweeps_;
    std::set<std::pair<std::string, StoryId>> tier_swept_;
    std::vector<std::shared_ptr<Claim>> stopped_migrations_;
    struct CompactionJob;
    absl::StatusOr<CompactionResult> runCompaction(const CompactionPolicy& policy, CompactionJob job);
    absl::Status compactionStep(std::string_view step) const;
    bool waitCompactionTurn(uint64_t bytes, const CompactionPolicy& policy);
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
        uint64_t revision{};
        // Superseded inputs of the story by their name without extension, to the output that replaced them.
        std::map<std::string, std::string> superseded;
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
    std::map<std::string, std::weak_ptr<Claim>> claims_;
    std::map<std::string, std::shared_ptr<PosixTier>> tiers_;
    mutable std::mutex tier_table_mutex_;
    std::map<std::string, std::future<absl::Status>> tier_unlinks_;
    std::map<std::string, std::chrono::steady_clock::time_point> tier_unlink_deadlines_;
    std::map<std::string, std::shared_ptr<std::vector<std::future<absl::Status>>>> unlink_results_;
    std::string deployment_;
    bool migration_stopped_{};
    uint64_t deletion_generation_{};
    size_t deletion_applied_{};
    mutable std::mutex mutex_;
    std::map<StoryId, std::optional<Hlc>> anchors_;
    mutable std::map<StoryId, Hlc> watermarks_;
    mutable std::unique_ptr<ArchiveReaderPool> readers_;
    std::chrono::milliseconds archive_read_timeout_{30000};
    Hooks hooks_;
    // Set once a manifest append or fsync failed or a switch could not be made durable; cleared only by reopening.
    bool compaction_stopped_{};
    size_t next_story_{};
    std::set<StoryId> swept_;
    std::atomic<StoryId> compacting_{};
    // Publishes in their codec write; a compaction codec call waits until none is running.
    std::mutex admission_;
    std::condition_variable admission_changed_;
    size_t publishing_{};
    bool stop_compaction_{};
    double io_tokens_{};
    std::chrono::steady_clock::time_point io_refilled_{};
};
} // namespace chronolog
