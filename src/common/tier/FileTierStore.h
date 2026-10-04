#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <set>
#include <span>
#include "common/tier/ChunkCodec.h"
#include "common/tier/ManifestLog.h"
#include "common/tier/PosixTier.h"
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
// One scrubber pass (I13.17). `through` is the highest durable own seq when the pass started; `marked` says the
// validated mark now names it.
struct ScrubResult
{
    size_t validated{}, skipped{}, lost{}, rolled_back{}, slow_failed{};
    uint64_t through{};
    bool marked{};
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
        // Replaces the result of one slow-tier system call of the scrubber's LOST verdict ("lost-open",
        // "lost-marker") with the returned errno; zero runs the real call.
        std::function<int(std::string_view)> tier_errno;
        std::function<absl::Status(std::string_view)> compaction_step;
        // Runs before the scrubber looks at each planned file; a non-OK result ends the pass without a mark.
        std::function<absl::Status(std::string_view)> scrub_step;
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
    // Validates every own effective Published file on `local` (on slow tiers too when slow_tiers is set, where a
    // failure is only reported), reading at most io_bytes_per_sec (zero is unpaced), then replaces the own validated
    // mark. A missing or corrupt file becomes Lost, or its own compaction output is rolled back, under the store
    // mutex after a manifest re-sync; a file claimed, superseded, Deleted or moved since the pass planned it is
    // skipped and does not hold the mark back. A rollback's inputs are validated without the mutex; when the switch,
    // an input's claim or its pending unlink changed meanwhile, the file is skipped and the pass writes no mark.
    // A slow-tier file (I13.15) is recorded Lost only by its owner, only when it is missing or corrupt through the
    // availability epoch's descriptor with the marker re-read through the same descriptor afterwards, a manifest
    // re-sync still resolves it to that tier, and the same verdict repeats in a later pass at least
    // slow_verdict_interval after the first, under a probe that started after the first verdict. Any other failure
    // (ESTALE, EIO, a timeout, a marker that no longer verifies) ends the epoch, withdraws a pending verdict and is
    // only reported in slow_failed.
    absl::StatusOr<ScrubResult>
    scrubOnce(uint64_t io_bytes_per_sec,
              bool slow_tiers = false,
              std::chrono::milliseconds slow_verdict_interval = std::chrono::milliseconds(0));
    void stopScrub();
    // I13.16: `local` keeps hard_stop_reserve_bytes free. Below it publish of a new window and compaction output are
    // refused RESOURCE_EXHAUSTED; a duplicate transfer still settles, and manifest appends, tombstones, destroy and
    // migration continue inside the reserve. Zero disables. free_bytes replaces statvfs of the root, for tests.
    using FreeBytes = std::function<absl::StatusOr<uint64_t>()>;
    void setHardStopReserve(uint64_t bytes, FreeBytes free_bytes = {});
    absl::Status hardStop() const;
    // The smallest reserve that still lets a full `local` drain: twice this writer's manifest log and snapshot (a
    // manifest compaction writes a whole snapshot temporary) plus one migration pass of lines.
    absl::StatusOr<uint64_t> hardStopReserveBound() const;
    uint64_t hardStopReserve() const;
    absl::Status configureTiers(std::string deployment,
                                std::vector<TierConfig> tiers,
                                size_t threads = 2,
                                std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    absl::Status probeTiers(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) const;
    absl::StatusOr<size_t> migrateOnce(const std::string& destination,
                                       std::optional<uint32_t> source_rank = std::nullopt,
                                       int64_t before_end_ns = INT64_MAX,
                                       uint64_t max_bytes = UINT64_MAX,
                                       uint64_t* copied_bytes = nullptr);
    struct TierUsage
    {
        bool available{};
        uint64_t used_bytes{}, total_bytes{}, free_bytes{};
    };
    absl::StatusOr<TierUsage> tierUsage(const std::string& name) const;
    std::map<StoryId, size_t> pendingTierDeletions() const;
    bool migrationStopped() const;
    absl::Status cleanupMigrations();
    absl::Status sweepTiers();
    absl::Status writeTierReplicas();
    absl::Status awaitTierUnlinksForTesting(std::chrono::milliseconds timeout);
    // Manifest records visited so far by story view builds, view lookups and watermark walks.
    uint64_t viewWorkForTesting() const;
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
    absl::StatusOr<std::vector<Event>>
    validateBytes(const ManifestRecord& record, ChunkBytes& bytes, std::optional<FileChecksum> checksum) const;
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
    absl::Status rollBack(const ManifestRecord& record, Hlc w, const std::vector<ManifestRecord>& inputs, bool running);
    absl::Status recordLost(ManifestRecord record, Hlc w);
    // kSkipped: the record is no longer the one the pass planned. kDeferred: it still is, but its switch, an input's
    // claim or pending unlink changed while the inputs were validated; the next pass decides.
    enum class ScrubDecision
    {
        kWritten,
        kSkipped,
        kDeferred,
    };
    absl::StatusOr<ScrubDecision> scrubRollbackOrLose(std::unique_lock<std::mutex>& lock,
                                                      const ManifestRecord& record,
                                                      const std::function<bool(const ManifestIndex&)>& planned);
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
        // effective is ordered by key (the file name, or writer, chunk and start for a record without a file) and
        // keys holds that key per position. A view whose story only gained records since it was built takes those
        // records alone, so a publish does not pay for the files already in the manifest.
        std::vector<ManifestRecord> effective;
        std::vector<std::string> keys;
        std::vector<size_t> by_start;
        // Keys applied since the watermark was last computed; overflowed means too many to keep, so recompute.
        std::vector<std::string> touched;
        bool touched_overflowed{};
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
    void applyToView(const ManifestIndex& index, StoryView& view, const ManifestRecord& record) const;
    const ManifestRecord* inView(const StoryView& view, const std::string& file) const;
    std::vector<ManifestRecord> effective(const ManifestIndex& index, StoryId story) const;
    Hlc watermark(const ManifestIndex& index, StoryId story) const;
    bool known(const ManifestIndex& index, StoryId story) const;

    bool read_only_{};
    std::chrono::milliseconds manifest_poll_{1000};
    mutable bool polled_{};
    mutable std::map<StoryId, StoryView> views_;
    // Records a view build, a view lookup or a watermark walk visited, so a gate can assert the work of one publish.
    mutable uint64_t view_work_{};
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
    // First LOST verdicts of slow-tier files awaiting their repeat (I13.15), under mutex_.
    struct SlowVerdict
    {
        std::string tier_uuid;
        uint64_t probes{};
        std::chrono::steady_clock::time_point at;
    };
    std::map<std::string, SlowVerdict> slow_verdicts_;
    std::string deployment_;
    mutable std::mutex reserve_mutex_;
    uint64_t hard_stop_reserve_{};
    FreeBytes free_bytes_;
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
    std::mutex scrub_mutex_;
    std::condition_variable scrub_changed_;
    bool stop_scrub_{};
    double io_tokens_{};
    std::chrono::steady_clock::time_point io_refilled_{};
};
} // namespace chronolog
