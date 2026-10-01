#pragma once

#include "chronolog/types.h"
#include <filesystem>
#include <mutex>

namespace chronolog
{
struct ManifestIndex
{
    std::vector<ManifestRecord> records;
    std::map<StoryId, Hlc> watermarks;
};

class ManifestLog
{
public:
    static absl::StatusOr<std::unique_ptr<ManifestLog>> Open(std::filesystem::path root, std::string writer);
    static std::unique_ptr<ManifestLog> OpenReadOnly(std::filesystem::path root);
    ~ManifestLog();
    absl::Status append(ManifestRecord record);
    absl::Status rememberWatermark(StoryId story, Hlc watermark);
    absl::StatusOr<ManifestIndex> load() const;
    absl::Status compact();
    std::filesystem::path logPath() const;
    std::filesystem::path snapshotPath() const;

private:
    ManifestLog(std::filesystem::path directory, std::string writer, int fd);
    absl::Status appendLine(std::string line);
    std::filesystem::path directory_;
    std::string writer_;
    int fd_;
    mutable std::mutex mutex_;
};
} // namespace chronolog
