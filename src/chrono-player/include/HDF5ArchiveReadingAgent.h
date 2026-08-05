#ifndef CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H
#define CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H

#include <set>

#include <ArchiveManifest.h>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <list>
#include <string>
#include <map>
#include <filesystem>
#include <thallium.hpp>
#include <chrono>
#include <vector>
#include <algorithm>
#include <atomic>
#include <unistd.h> // Required for access()

#include <chrono_monitor.h>
#include <StoryChunkIngestionQueue.h>

namespace tl = thallium;
namespace fs = std::filesystem;

namespace chronolog
{

// One indexed archive file. end_time comes from the archive manifest; it is 0
// when the index was built by scanning the directory, because a file name carries
// only the window start. A known end lets a read skip a file without opening it,
// which is the difference between one HDF5 open per candidate file and none.
struct ArchivedFileEntry
{
    // Every file published for this window, in the order the manifest records
    // them: the base file first, then any rotations. A rotation is never part 2 of
    // a chunk -- the writer rotates only when the base name is taken -- so these
    // all describe the SAME window and are read together into one StoryChunk,
    // which is keyed by EventSequence and therefore de-duplicates them.
    std::vector<std::string> paths;
    uint64_t end_time = 0; // 0 == unknown (a scanned file name carries no end)
};

class HDF5ArchiveReadingAgent
{
    // File system state tracking for polling
    struct FileInfo
    {
        std::string path;
        fs::file_time_type last_modified;
        std::uintmax_t file_size;
        bool is_directory;

        FileInfo()
            : file_size(0)
            , is_directory(false)
        {}
        FileInfo(const std::string& p, const fs::file_time_type& lm, std::uintmax_t fs, bool is_dir)
            : path(p)
            , last_modified(lm)
            , file_size(fs)
            , is_directory(is_dir)
        {}
    };

    // Directory caching for performance optimization
    struct DirectoryCache
    {
        fs::path path;
        int64_t last_modified_ns;
        int64_t last_check_time_ns;
        bool has_changes;

        DirectoryCache()
            : last_modified_ns(0)
            , last_check_time_ns(0)
            , has_changes(false)
        {}
        DirectoryCache(const fs::path& p, int64_t lm_ns, int64_t lc_ns, bool changes)
            : path(p)
            , last_modified_ns(lm_ns)
            , last_check_time_ns(lc_ns)
            , has_changes(changes)
        {}
    };

public:
    explicit HDF5ArchiveReadingAgent(std::string const& archive_path,
                                     bool use_polling = true,
                                     std::chrono::milliseconds monitoring_interval = std::chrono::milliseconds(5000),
                                     bool manifest_enabled = true,
                                     std::chrono::milliseconds manifest_poll_interval = std::chrono::milliseconds(1000))
        : archive_path_(fs::absolute(expandTilde(fs::path(archive_path))).make_preferred().string())
        , use_polling_(use_polling)
        , monitoring_interval_(monitoring_interval)
        , shutdown_requested_(false)
        , manifest_enabled_(manifest_enabled)
        , manifest_poll_interval_(manifest_poll_interval)
    {}

    ~HDF5ArchiveReadingAgent() { shutdown(); }

    int initialize()
    {
        LOG_INFO("[HDF5ArchiveReadingAgent] Initializing, scanning archive path {} recursively to create the map ...",
                 archive_path_);
        // Prefer the manifest: it names exactly what was published, carries each
        // file's window end, and costs one sequential read instead of a recursive
        // walk of the whole archive. The scan stays as the fallback for archives
        // written before the manifest existed, or when it is unreadable -- losing
        // the manifest must degrade performance, never correctness.
        if(!manifest_enabled_)
        {
            LOG_INFO("[HDF5ArchiveReadingAgent] manifest_enabled is false; indexing {} by directory scan",
                     archive_path_);
            createStartTimeFileNameMap();
        }
        else if(loadIndexFromManifest() == 0)
        {
            manifest_mode_ = true;
        }
        else
        {
            // Worth a warning rather than a note: the Grapher writes the manifest
            // into chrono_grapher's "hdf5_archive_dir" and the Player reads it from
            // chrono_player's "story_files_dir". Those are independent keys, so a
            // mismatch looks exactly like "no manifest yet" -- the Player quietly
            // scans and never restores anything, which is the failure this message
            // exists to make visible.
            LOG_WARNING("[HDF5ArchiveReadingAgent] No usable archive manifest in {}; falling back to a recursive "
                        "directory scan. If the Grapher is writing one, check that chrono_player's "
                        "'story_files_dir' names the same directory as chrono_grapher's 'hdf5_archive_dir'",
                        archive_path_);
            createStartTimeFileNameMap();
        }
        return setUpFsMonitoring();
    }

    int shutdown()
    {
        if(shutdown_requested_.load() == true)
        {
            LOG_INFO("[HDF5ArchiveReadingAgent] Shutdown already requested. Skipping shutdown request.");
            return 0;
        }
        shutdown_requested_.store(true);
        archive_dir_monitoring_stream_->join();
        archive_dir_monitoring_thread_->join();
        return 0;
    }

    // Applies manifest records appended since the last call and returns how many
    // were consumed. Public because the monitoring thread is not the only sensible
    // driver: a test wants to step it, and stepping it is what makes the
    // byte-offset bookkeeping observable at all.
    int pollManifestTail();

    // One log's worth of pollManifestTail.
    int pollOneManifestLog(std::string const& log_path);

    int readStoryChunkFile(const ChronicleName&,
                           const StoryName&,
                           uint64_t,
                           uint64_t,
                           std::list<StoryChunk*>&,
                           const std::string&);

    int readArchivedStory(const ChronicleName&,
                          const StoryName&,
                          uint64_t,
                          uint64_t,
                          std::list<StoryChunk*>&,
                          bool = false);

    static std::string getChronicleName(const std::string& file_name)
    {
        // Example file name: /home/kfeng/chronolog/Debug/output/chronicle_0_0.story_0_0.1736806500.vlen.h5
        std::string base_name = fs::path(file_name).filename().string();
        std::string chronicle_name = base_name.substr(0, base_name.find_first_of('.'));
        return chronicle_name;
    }

    static std::string getStoryName(const std::string& file_name)
    {
        // Example file name: /home/kfeng/chronolog/Debug/output/chronicle_0_0.story_0_0.1736806500.vlen.h5
        std::string base_name = fs::path(file_name).filename().string();
        size_t first_dot = base_name.find_first_of('.');
        size_t second_dot = base_name.find_first_of('.', first_dot + 1);
        std::string story_name = base_name.substr(first_dot + 1, second_dot - first_dot - 1);
        return story_name;
    }

    static uint64_t getStartTime(const std::string& file_name)
    {
        // Example file name: /home/kfeng/chronolog/Debug/output/chronicle_0_0.story_0_0.1736806500.vlen.h5
        //        LOG_DEBUG("[HDF5ArchiveReadingAgent] Extracting start time from file name: {}", file_name);
        std::string base_name = fs::path(file_name).filename().string();
        size_t first_dot = base_name.find_first_of('.');
        size_t second_dot = base_name.find_first_of('.', first_dot + 1);
        size_t third_dot = base_name.find_first_of('.', second_dot + 1);
        std::string start_time_str = base_name.substr(second_dot + 1, third_dot - second_dot - 1);
        //        LOG_DEBUG("[HDF5ArchiveReadingAgent] Extracted start time: {}", start_time_str);
        uint64_t start_time_in_ns = 0;
        try
        {
            start_time_in_ns = std::stoull(start_time_str) * 1000000000;
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Use start time={} for file map", start_time_in_ns);
        }
        catch(const std::exception& e)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to convert start time string '{}' to uint64_t: {}",
                      start_time_str,
                      e.what());
        }
        return start_time_in_ns;
    }

private:
    fs::path expandTilde(fs::path path)
    {
        if(!path.empty() && path.string()[0] == '~')
        {
            const char* home = getenv("HOME");
            if(home)
            {
                fs::path expanded_path = fs::path(home) / path.string().substr(1);
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Expanding archive path from {} to {}",
                          path.string(),
                          expanded_path.string());
                return expanded_path;
            }
            else
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] HOME environment variable is not set. Cannot expand tilde in "
                          "path: {}",
                          path.string());
                return path; // Return the original path if HOME is not set
            }
        }
        return path;
    }

    bool isValidArchiveFile(const std::string& file_name)
    {
        fs::path expanded_full_path = fs::absolute(fs::path(file_name));
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Checking if file {} is a valid archive file.",
                  expanded_full_path.string());

        // Quick check: file extension first
        if(expanded_full_path.extension() != ".h5")
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] File {} is not an HDF5 file. Skipping this file.",
                      expanded_full_path.string());
            return false;
        }

        // Check if file exists and is readable using access()
        if(access(expanded_full_path.c_str(), R_OK) != 0)
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] File {} is not readable. Skipping this file.",
                      expanded_full_path.string());
            return false;
        }

        // Verify it's a regular file using filesystem
        fs::directory_entry entry(expanded_full_path);
        std::error_code ec;
        if(!entry.is_regular_file(ec))
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] File {} is not a regular file. Skipping this file.",
                      expanded_full_path.string());
            return false;
        }

        return true;
    }

    int setUpFsMonitoring();

    // Inotify-based monitoring methods
    void addRecursiveWatch(int inotify_fd, const std::string& path, std::map<int, std::string>& wd_to_path);
    int inotifyMonitoringThreadFunc();

    // Polling-based monitoring methods
    int pollingMonitoringThreadFunc();
    void scanFileSystem();
    bool hasFileSystemChanged();
    bool hasDirectoryChangedOptimizedRecursive(const fs::path& dir_path, int64_t last_scan_ns, std::error_code& ec);
    void updateFileState();
    std::vector<FileInfo> getCurrentFileState();

    // Directory caching methods
    int64_t getDirectoryModificationTime(const fs::path& dir_path, std::error_code& ec);
    bool hasDirectoryChangedWithCache(const fs::path& dir_path, int64_t last_scan_ns, std::error_code& ec);
    void
    updateDirectoryCache(const fs::path& dir_path, int64_t last_modified_ns, int64_t check_time_ns, bool has_changes);
    void clearDirectoryCache();

    // Builds the index from the archive manifest. Returns 0 on success, -1 when
    // there is no manifest to read (the caller then scans).
    int loadIndexFromManifest()
    {
        ArchiveManifest manifest(archive_path_);
        if(manifest.load() != CL_SUCCESS)
        {
            return -1;
        }
        std::vector<ArchiveManifestRecord> const records = manifest.records();
        if(records.empty())
        {
            return -1;
        }

        // A deletion arrives as a later record naming the same file, so collect
        // those first and skip the publications they supersede -- otherwise the
        // index would name files that are no longer on disk.
        std::set<std::string> deleted_files;
        for(ArchiveManifestRecord const& record: records)
        {
            if(record.state == ManifestState::DELETED && !record.file.empty())
            {
                deleted_files.insert(record.file);
            }
        }

        std::size_t indexed = 0;
        for(ArchiveManifestRecord const& record: records)
        {
            if(record.state != ManifestState::PUBLISHED || record.file.empty())
            {
                continue; // empty/failed windows have no file to read
            }
            if(deleted_files.count(record.file) > 0)
            {
                continue;
            }
            // is_rotation: the manifest is authoritative about what belongs to a
            // window, so a rotation it lists is indexed alongside the base file
            // rather than skipped by the file-name heuristic.
            if(addFileToStartTimeFileNameMap((fs::path(archive_path_) / record.file).string(),
                                             record.end,
                                             /*is_rotation=*/true) == 0)
            {
                indexed++;
            }
        }

        // Success is "the manifest was readable", NOT "it produced entries". A
        // manifest whose files have all been deleted legitimately indexes nothing,
        // and treating that as absent would fall back to the scan and re-index the
        // very files the deletions removed.
        LOG_INFO("[HDF5ArchiveReadingAgent] Indexed {} archive file(s) from {} manifest record(s) in {} "
                 "(no directory scan)",
                 indexed,
                 records.size(),
                 archive_path_);
        return 0;
    }

    int createStartTimeFileNameMap()
    {
        // iterate over the HDF5 files in the archive directory to get the list of files
        // update the start_time_file_name_map_ with the start time and file name
        std::error_code ec;
        auto it = fs::recursive_directory_iterator(archive_path_, fs::directory_options::skip_permission_denied, ec);
        if(ec)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to iterate recursively over archive directory '{}': {}",
                      archive_path_,
                      ec.message());
            return -1; // Return error code on failure
        }
        std::string file_name, chronicle_name, story_name;
        for(const auto& entry: it)
        {
            if(ec)
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] Error accessing path: {}", ec.message());
                // Clear the error to continue iteration on other branches.
                ec.clear();
                continue;
            }

            file_name = entry.path().string();
            addFileToStartTimeFileNameMap(file_name);
        }

        LOG_DEBUG("[HDF5ArchiveReadingAgent] Created start_time_file_name_map_ with {} entries.",
                  start_time_file_name_map_.size());
        return 0;
    }

    void printStartTimeFileNameMapEntryCount(const std::string& chronicle_name, const std::string& story_name)
    {
        auto chronicle_story_it = start_time_file_name_map_.find(std::make_pair(chronicle_name, story_name));
        if(chronicle_story_it != start_time_file_name_map_.end())
        {
            LOG_DEBUG(
                    "[HDF5ArchiveReadingAgent] start_time_file_name_map_ has {} entries, entry: <{}, {}> has {} files",
                    start_time_file_name_map_.size(),
                    chronicle_name,
                    story_name,
                    chronicle_story_it->second.size());
        }
        else
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] start_time_file_name_map_ has {} entries, entry: <{}, {}> does not "
                      "exist",
                      start_time_file_name_map_.size(),
                      chronicle_name,
                      story_name);
        }
    }

    // is_rotation says the file is an additional write of a window already indexed
    // (or about to be); it is appended to that window's list rather than replacing
    // it. Scanned files never set it: the scan path keeps its historical behaviour
    // of indexing only base files.
    int addFileToStartTimeFileNameMap(const std::string& file_name, uint64_t end_time = 0, bool is_rotation = false)
    {
        std::lock_guard<std::mutex> lock(start_time_file_name_map_mutex_);
        if(!isValidArchiveFile(file_name))
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Invalid archive file: {}. Skipping this file.", file_name);
            return -1; // Skip invalid files
        }
        std::string chronicle_name = getChronicleName(file_name);
        std::string story_name = getStoryName(file_name);
        uint64_t start_time = getStartTime(file_name);
        if(start_time == 0)
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Failed to extract start time from file: {}. Skipping this file.",
                      file_name);
            return -1; // Skip files with invalid start time
        }
        std::string file_name_number = fs::path(file_name).replace_extension("").extension().string().substr(1);
        if(!is_rotation && std::all_of(file_name_number.begin(), file_name_number.end(), ::isdigit))
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] {} is an auxiliary file. Skipping this file.", file_name);
#ifndef NDEBUG
            printStartTimeFileNameMapEntryCount(chronicle_name, story_name);
#endif
            return -1; // Skip files that already exist in the map
        }
        ArchivedFileEntry& entry = start_time_file_name_map_[std::make_pair(chronicle_name, story_name)][start_time];
        if(std::find(entry.paths.begin(), entry.paths.end(), file_name) == entry.paths.end())
        {
            entry.paths.push_back(file_name);
        }
        if(end_time != 0)
        {
            entry.end_time = end_time;
        }
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Added file {} to start_time_file_name_map_.", file_name);
#ifndef NDEBUG
        printStartTimeFileNameMapEntryCount(chronicle_name, story_name);
#endif
        return 0;
    }

    // is_rotation mirrors the add path: the manifest knows a rotation belongs to a
    // window, so a deletion it reports removes just that file and leaves the
    // window's other files indexed.
    int removeFileFromStartTimeFileNameMap(const std::string& file_name, bool is_rotation = false)
    {
        std::lock_guard<std::mutex> lock(start_time_file_name_map_mutex_);
        std::string chronicle_name = getChronicleName(file_name);
        std::string story_name = getStoryName(file_name);
        uint64_t start_time = getStartTime(file_name);
        if(start_time == 0)
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Failed to extract start time from file: {}. Skipping this file.",
                      file_name);
            return -1; // Skip files with invalid start time
        }
        std::string file_name_number = fs::path(file_name).replace_extension("").extension().string().substr(1);
        if(!is_rotation && std::all_of(file_name_number.begin(), file_name_number.end(), ::isdigit))
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] {} is an auxiliary file. Skipping this file.", file_name);
#ifndef NDEBUG
            printStartTimeFileNameMapEntryCount(chronicle_name, story_name);
#endif
            return -1; // Skip files that already exist in the map
        }
        auto chronicle_story_pair = std::make_pair(chronicle_name, story_name);
        auto chronicle_story_it = start_time_file_name_map_.find(chronicle_story_pair);
        if(chronicle_story_it != start_time_file_name_map_.end())
        {
            auto window_it = chronicle_story_it->second.find(start_time);
            if(window_it != chronicle_story_it->second.end())
            {
                std::vector<std::string>& paths = window_it->second.paths;
                paths.erase(std::remove(paths.begin(), paths.end(), file_name), paths.end());
                // The window entry only goes when nothing is left to read from it;
                // dropping it while a sibling rotation survived would hide that file.
                if(paths.empty())
                {
                    chronicle_story_it->second.erase(window_it);
                }
            }
            // Remove the chronicle-story entry if no more files exist for it
            if(chronicle_story_it->second.empty())
            {
                start_time_file_name_map_.erase(chronicle_story_it);
            }
        }
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Removed file {} from start_time_file_name_map_.", file_name);
#ifndef NDEBUG
        printStartTimeFileNameMapEntryCount(chronicle_name, story_name);
#endif
        return 0;
    }

    int renameFileInStartTimeFileNameMap(const std::string& old_file_name, const std::string& new_file_name)
    {
        removeFileFromStartTimeFileNameMap(old_file_name);
        addFileToStartTimeFileNameMap(new_file_name);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Renamed file {} to {} in start_time_file_name_map_.",
                  old_file_name,
                  new_file_name);
        return 0;
    }

    std::string archive_path_;
    std::map<std::pair<std::string, std::string>, std::map<uint64_t, ArchivedFileEntry>> start_time_file_name_map_;
    std::mutex start_time_file_name_map_mutex_;
    tl::managed<tl::xstream> archive_dir_monitoring_stream_;
    tl::managed<tl::thread> archive_dir_monitoring_thread_;

    // Feature flag and monitoring configuration
    bool use_polling_;
    std::chrono::milliseconds monitoring_interval_;
    std::chrono::system_clock::time_point last_scan_time_;

    // File system state tracking for polling
    std::map<std::string, FileInfo> previous_file_state_;
    std::mutex file_state_mutex_;

    // Directory caching for performance optimization
    std::map<fs::path, DirectoryCache> directory_cache_;
    std::mutex directory_cache_mutex_;

    // Thread control
    std::atomic<bool> shutdown_requested_;
    bool manifest_enabled_ = true;
    // True once the index was built from the manifest; that is what decides
    // whether the monitoring thread polls the log or walks the directory.
    bool manifest_mode_ = false;
    // Tick used instead of monitoring_interval_ once the index is manifest-backed:
    // reading an append-only tail is cheap enough to do far more often than the
    // directory diff it replaces.
    std::chrono::milliseconds manifest_poll_interval_{1000};
    // How much of one manifest log has already been applied.
    struct ManifestLogCursor
    {
        // Only ever advanced past a COMPLETE line: the Grapher appends without
        // holding a lock the Player can see, so a poll can land mid-record.
        std::streamoff offset = 0;
        // Size alone cannot detect a compaction: it truncates the log and the
        // Grapher immediately appends again, so the file can be back at exactly the
        // offset already consumed while holding entirely different records.
        std::filesystem::file_time_type mtime{};
    };

    // One cursor per manifest log, keyed by path. There is a log per Grapher (see
    // ArchiveManifest), so the Player follows several tails at once and each has
    // its own position.
    std::map<std::string, ManifestLogCursor> manifest_log_cursors_;
};

} // namespace chronolog

#endif //CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H
