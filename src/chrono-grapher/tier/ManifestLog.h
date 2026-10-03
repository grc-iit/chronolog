#pragma once

#include "chronolog/types.h"
#include <filesystem>
#include <functional>
#include <sys/stat.h>
#include <sys/types.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace chronolog
{
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
struct ManifestIndex
{
    std::vector<ManifestRecord> records;
    std::map<std::string, FilePhysicalBounds> physical_bounds;
    std::map<StoryId, Hlc> watermarks;
    std::set<StoryId> tombstoned;
    // Positions in records per story, so a story's view is rebuilt only when it changed and never by scanning the
    // whole manifest. generation changes whenever the index is rebuilt from scratch.
    std::map<StoryId, std::vector<size_t>> by_story;
    std::set<StoryId> without_physical_policy;
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
    absl::Status append(ManifestRecord record, std::optional<PhysicalBounds> bounds = std::nullopt);
    absl::Status rememberWatermark(StoryId story, Hlc watermark);
    // Fsync'd before it returns. Compaction keeps the line, so the story stays tombstoned for good (I13.11).
    absl::Status appendTombstone(StoryId story);
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
    };
    struct WriterCursors
    {
        Cursor log, snapshot;
    };
    absl::Status rebuild() const;
    absl::Status advance(bool& changed) const;
    absl::Status applyLine(const std::string& writer, const std::string& line, ManifestIndex& index) const;
    std::filesystem::path directory_;
    std::string writer_;
    int fd_;
    PathStat path_stat_;
    mutable std::mutex mutex_;
    mutable ManifestIndex cache_;
    mutable std::map<std::string, WriterCursors> cursors_;
    mutable bool synced_{};
    mutable uint64_t generations_{};
};
} // namespace chronolog
