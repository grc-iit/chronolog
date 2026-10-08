#ifndef CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H
#define CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <ArchiveManifest.h>
#include <chrono_monitor.h>
#include <StoryChunk.h>

namespace fs = std::filesystem;

namespace chronolog
{

// Reads a story's events back from the HDF5 archive.
//
// The files of a story are found through the archive manifest (see
// ArchiveManifest.h): every grapher appends a record for each file it
// publishes and for each story or chronicle whose files it deletes. A replay
// first reads what the logs gained since the last replay, so a file is found as
// soon as its record is appended, without listing any directory. Archives
// written before 3.3.0, which have no manifest, are not read.
class HDF5ArchiveReadingAgent
{
public:
    explicit HDF5ArchiveReadingAgent(std::string const& archive_path)
        : archive_path_(fs::absolute(expandTilde(fs::path(archive_path))).make_preferred().string())
    {}

    ~HDF5ArchiveReadingAgent() { shutdown(); }

    // Reads the manifest.
    int initialize();

    int shutdown() { return 0; }

    int readStoryChunkFile(const ChronicleName&,
                           const StoryName&,
                           uint64_t,
                           uint64_t,
                           std::list<StoryChunk*>&,
                           const std::string&);

    // Every event of the story in [start_time, end_time) the archive holds.
    // CL_SUCCESS when every file that should hold some was read, including when
    // there are none; an error when a file could not be read or the archive
    // directory is missing, with whatever the readable files held in the list.
    // A file removed since it was recorded (its story was destroyed) is
    // skipped and not an error.
    int readArchivedStory(const ChronicleName&, const StoryName&, uint64_t, uint64_t, std::list<StoryChunk*>&);

private:
    // One file the manifest recorded: where it is, and the logs that recorded
    // it (ids into manifest_tails_). A story destroyed and created again can
    // have the same file name recorded by another grapher, so one path can
    // come from several logs, and each log's deletion takes only its own claim.
    struct RecordedFile
    {
        std::string path;
        std::vector<uint32_t> logs;
    };

    // The files recorded for one start time, and the latest end among them:
    // the base file of a window, its later writes, a salvage file.
    struct RecordedRange
    {
        uint64_t end = 0;
        std::vector<RecordedFile> files;
    };

    // A story's recorded files by start time (ns), and the longest range among
    // them: a range reaching into a replay starts at most that long before it.
    struct RecordedStory
    {
        uint64_t max_span = 0;
        std::map<uint64_t, RecordedRange> ranges;
    };

    using StoryKey = std::pair<ChronicleName, StoryName>;

    fs::path expandTilde(fs::path path)
    {
        if(!path.empty() && path.string()[0] == '~')
        {
            const char* home = getenv("HOME");
            if(home)
            {
                return fs::path(home) / path.string().substr(1);
            }
            LOG_ERROR("[HDF5ArchiveReadingAgent] HOME environment variable is not set. Cannot expand tilde in "
                      "path: {}",
                      path.string());
        }
        return path;
    }

    // Applies whatever the manifest logs gained since the last call, and
    // follows any log that appeared since. false when a log or the manifest
    // directory could not be read: the index may miss files. Caller holds
    // index_mutex_. log_count, when given, receives the number of logs.
    bool refreshFromManifest(std::size_t* log_count = nullptr);
    void applyRecord(ArchiveManifestRecord const& record, uint32_t log);
    // Drops what a deletion covers from the index. Every grapher records a
    // destroy in its own log, after that log's records of the files, so a
    // deletion takes its log off each file it covers, and a file no other log
    // recorded is dropped. Nothing is looked up (see the .cpp).
    void applyDeletion(ArchiveManifestRecord const& record, uint32_t log);

    std::string archive_path_;
    std::mutex index_mutex_;
    // log path -> id, and the tail of each log by id
    std::map<std::string, uint32_t> log_ids_;
    std::vector<ArchiveManifestTail> manifest_tails_;
    // (chronicle, story) -> the files recorded for it
    std::map<StoryKey, RecordedStory> recorded_files_;
};

} // namespace chronolog

#endif //CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H
