#pragma once

#include "chronolog/types.h"
#include <atomic>
#include <filesystem>
#include <functional>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace chronolog
{
struct FileChecksum
{
    uint64_t bytes{};
    uint32_t crc32c{};
};
struct PhysicalBounds
{
    int64_t min_lo{}, max_hi{};
    bool unbounded{};
};
struct FilePhysicalBounds
{
    PhysicalBounds bounds;
    StoryId story_id{};
    Hlc start, end;
    uint64_t event_count{};
};
// One compact_v1 line: the output replaces every input in the effective view at once (I13.12).
struct CompactionSwitch
{
    std::string writer, op;
    StoryId story_id{};
    std::vector<ManifestRecord> inputs;
    ManifestRecord output;
    std::optional<PhysicalBounds> bounds;
    Hlc w_floor;
    std::optional<FileChecksum> checksum;
};
// Compaction outputs carry a reserved name that ordinary orphan adoption cannot parse; the name encodes the
// writer and the output window so recovery can judge an unreferenced output without its switch line.
struct CompactionOutput
{
    std::string writer, op;
    Hlc start, end;
};
std::string CompactionOutputName(const CompactionOutput& output, const std::string& extension);
std::optional<CompactionOutput> ParseCompactionOutput(const std::filesystem::path& relative);
std::string CompactionTemporaryPrefix(const std::string& writer);
struct MigrationLocation
{
    std::string writer, file, tier, tier_uuid, token;
    StoryId story_id{};
    uint32_t rank{};
    FileChecksum checksum;
};
std::optional<std::string> ArchiveFileWriter(const std::filesystem::path& file);
struct ManifestIndex
{
    std::map<std::string, MigrationLocation> locations;
    std::map<std::pair<std::string, uint32_t>, MigrationLocation> migration_ranks;
    std::vector<ManifestRecord> records;
    std::map<std::string, FileChecksum> checksums;
    std::map<std::string, uint64_t> record_sequences;
    std::map<std::string, FilePhysicalBounds> physical_bounds;
    std::map<StoryId, Hlc> watermarks;
    std::set<StoryId> tombstoned;
    // Positions in records per story, so a story's view is rebuilt only when it changed and never by scanning the
    // whole manifest. generation changes whenever the index is rebuilt from scratch.
    std::map<StoryId, std::vector<size_t>> by_story;
    std::set<StoryId> without_physical_policy;
    // Switches by output file and the input files they supersede, whichever writer's records name those files.
    // A rolled back switch supersedes nothing and its output is no longer part of any view.
    std::map<std::string, CompactionSwitch> switches;
    std::map<std::string, std::string> superseded;
    std::set<std::string> rolled_back;
    // Changes whenever a switch or rollback of the story applies, so a cached story view is rebuilt.
    std::map<StoryId, uint64_t> revisions;
    uint64_t generation{};
};

class ManifestLog
{
public:
    // Metadata of a path as the attribute cache reports it, which on NFS can lag an append by acregmax. Change
    // detection never relies on it; it is injectable so a test can make it lie.
    using PathStat = std::function<int(const std::filesystem::path&, struct stat&)>;
    static absl::StatusOr<std::unique_ptr<ManifestLog>>
    Open(std::filesystem::path root, std::string writer, PathStat path_stat = {});
    static std::unique_ptr<ManifestLog> OpenReadOnly(std::filesystem::path root, PathStat path_stat = {});
    ~ManifestLog();
    absl::Status append(ManifestRecord record,
                        std::optional<PhysicalBounds> bounds = std::nullopt,
                        std::optional<FileChecksum> checksum = std::nullopt);
    std::optional<MigrationLocation> location(const std::string& file) const;
    std::optional<FileChecksum> checksum(const std::string& file) const;
    absl::Status rememberWatermark(StoryId story, Hlc watermark);
    // Fsync'd before it returns. Compaction keeps the line, so the story stays tombstoned for good (I13.11).
    absl::Status appendTombstone(StoryId story);
    absl::Status appendMigration(const MigrationLocation& location);
    absl::Status appendSwitch(const CompactionSwitch& change);
    absl::Status appendRollback(StoryId story, const std::string& output);
    // Fsyncs this writer's log. Superseded inputs are unlinked only after a switch line is durable through it.
    absl::Status syncOwn();
    // True once any append, fsync or truncation of this writer's log failed: the log may hold a complete line that
    // is not durable, so compaction stops until the writer reopens.
    bool failed() const { return failed_.load(); }
    // Replaces fsync of the writer log, for fault injection.
    void setSync(std::function<int(int)> sync);
    absl::StatusOr<ManifestIndex> load() const;
    // Reads only what every writer appended since the previous call and returns the cached index. The pointer stays
    // valid until the next sync, compact or load, and the caller serialises calls.
    absl::StatusOr<const ManifestIndex*> sync() const;
    const ManifestIndex* current() const { return &cache_; }
    absl::Status compact();
    std::filesystem::path logPath() const;
    std::filesystem::path snapshotPath() const;

private:
    ManifestLog(std::filesystem::path directory, std::string writer, int fd, PathStat path_stat);
    absl::Status appendLine(std::string line);
    struct Cursor
    {
        dev_t device{};
        ino_t inode{};
        off_t offset{};
        bool present{};
        std::string fingerprint;
    };
    struct WriterCursors
    {
        Cursor log, snapshot;
    };
    absl::Status rebuild() const;
    absl::Status advance() const;
    absl::Status applyLine(const std::string& writer, const std::string& line, ManifestIndex& index) const;
    absl::Status appendFramed(std::string_view key, const std::string& body);
    int sync(int fd) const { return sync_ ? sync_(fd) : ::fsync(fd); }
    std::filesystem::path directory_;
    std::string writer_;
    int fd_;
    PathStat path_stat_;
    std::function<int(int)> sync_;
    std::atomic<bool> failed_{};
    mutable std::mutex mutex_;
    mutable ManifestIndex cache_;
    mutable std::map<std::string, WriterCursors> cursors_;
    mutable bool synced_{};
    mutable uint64_t generations_{};
};
} // namespace chronolog
