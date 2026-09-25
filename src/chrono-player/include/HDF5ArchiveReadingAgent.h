#ifndef CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H
#define CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H

#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <list>
#include <string>
#include <map>
#include <filesystem>
#include <regex>
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
                                     uint64_t archive_window_secs = 30)
        : archive_path_(fs::absolute(expandTilde(fs::path(archive_path))).make_preferred().string())
        , use_polling_(use_polling)
        , monitoring_interval_(monitoring_interval)
        , archive_window_secs_(archive_window_secs)
        , shutdown_requested_(false)
    {}

    ~HDF5ArchiveReadingAgent() { shutdown(); }

    int initialize()
    {
        LOG_INFO("[HDF5ArchiveReadingAgent] Initializing, scanning archive path {} recursively to create the map ...",
                 archive_path_);
        createStartTimeFileNameMap();
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

    int readStoryChunkFile(const ChronicleName&,
                           const StoryName&,
                           uint64_t,
                           uint64_t,
                           std::list<StoryChunk*>&,
                           const std::string&);

    // readAuxFiles: also read the numbered files ({...}.vlen.1.h5, .2, ...) a
    // grapher writes when a window it already wrote gets more events, from a
    // late or re-sent keeper chunk. Once the keepers free those chunks, the
    // numbered files are the only copy of their events.
    int readArchivedStory(const ChronicleName&,
                          const StoryName&,
                          uint64_t,
                          uint64_t,
                          std::list<StoryChunk*>&,
                          bool readAuxFiles = true);

    // The directory listing this agent keeps can be stale: on a shared file
    // system a client caches directory attributes, so a file the grapher wrote
    // on another node is invisible here for as long as that cache lives. A
    // lookup by name does not go through it. For the windows a replay needs
    // beyond the newest file listed for the story, this asks the file system
    // for the name the grapher would have written, and adds what it finds to
    // the map. Bounded to the last few windows of the range.
    void probeForRecentFiles(ChronicleName const&, StoryName const&, uint64_t start_time, uint64_t end_time);

    // An archive file is named <chronicle>.<story>.<startSec>.vlen.h5, or
    // <...>.vlen.<n>.h5 for a later write of the same window. Chronicle and story
    // names may contain dots, so the name is read from the right, where every
    // field has a fixed form: the start second, "vlen" and the optional number.
    // What is left, "<chronicle>.<story>", is never split: a replay asks for one
    // known story and looks it up by the same string (storyPrefix).
    struct ArchiveFileName
    {
        std::string story_prefix;
        uint64_t start_time = 0; // ns
        bool numbered = false;
    };

    static std::string storyPrefix(std::string const& chronicle_name, std::string const& story_name)
    {
        return chronicle_name + "." + story_name;
    }

    // false for a name that is not an archive file's
    static bool parseArchiveFileName(std::string const& file_name, ArchiveFileName& parsed)
    {
        // the leading group is greedy, so the fixed fields are taken from the end
        static std::regex const pattern(R"(^(.+)\.([0-9]+)\.vlen(\.[0-9]+)?\.h5$)");
        std::string const base_name = fs::path(file_name).filename().string();
        std::smatch match;
        if(!std::regex_match(base_name, match, pattern))
        {
            return false;
        }
        try
        {
            parsed.start_time = std::stoull(match[2].str()) * 1000000000ULL;
        }
        catch(std::exception const& e)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Start time in file name {} is out of range: {}", base_name, e.what());
            return false;
        }
        parsed.story_prefix = match[1].str();
        parsed.numbered = match[3].matched;
        return true;
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
        std::string file_name;
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
        // the directory has been read: from here a story missing from the map
        // means nothing was archived for it, not that nobody has looked
        initial_scan_done_.store(true);
        return 0;
    }

    void printStartTimeFileNameMapEntryCount(const std::string& story_prefix)
    {
        auto story_it = start_time_file_name_map_.find(story_prefix);
        if(story_it != start_time_file_name_map_.end())
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] start_time_file_name_map_ has {} entries, entry: <{}> has {} files",
                      start_time_file_name_map_.size(),
                      story_prefix,
                      story_it->second.size());
        }
        else
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] start_time_file_name_map_ has {} entries, entry: <{}> does not exist",
                      start_time_file_name_map_.size(),
                      story_prefix);
        }
    }

    int addFileToStartTimeFileNameMap(const std::string& file_name)
    {
        std::lock_guard<std::mutex> lock(start_time_file_name_map_mutex_);
        if(!isValidArchiveFile(file_name))
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Invalid archive file: {}. Skipping this file.", file_name);
            return -1; // Skip invalid files
        }
        ArchiveFileName parsed;
        if(!parseArchiveFileName(file_name, parsed))
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] {} is not named like an archive file. Skipping this file.", file_name);
            return -1;
        }
        if(parsed.numbered)
        {
            // read through its window's base file, see readArchivedStory
            LOG_DEBUG("[HDF5ArchiveReadingAgent] {} is an auxiliary file. Skipping this file.", file_name);
            return -1;
        }
        start_time_file_name_map_[parsed.story_prefix][parsed.start_time] = file_name;
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Added file {} to start_time_file_name_map_.", file_name);
#ifndef NDEBUG
        printStartTimeFileNameMapEntryCount(parsed.story_prefix);
#endif
        return 0;
    }

    int removeFileFromStartTimeFileNameMap(const std::string& file_name)
    {
        std::lock_guard<std::mutex> lock(start_time_file_name_map_mutex_);
        ArchiveFileName parsed;
        if(!parseArchiveFileName(file_name, parsed) || parsed.numbered)
        {
            return -1; // never added to the map
        }
        auto story_it = start_time_file_name_map_.find(parsed.story_prefix);
        if(story_it != start_time_file_name_map_.end())
        {
            story_it->second.erase(parsed.start_time);
            // Remove the story's entry if no more files exist for it
            if(story_it->second.empty())
            {
                start_time_file_name_map_.erase(story_it);
            }
        }
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Removed file {} from start_time_file_name_map_.", file_name);
#ifndef NDEBUG
        printStartTimeFileNameMapEntryCount(parsed.story_prefix);
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
    // "<chronicle>.<story>" (storyPrefix) -> window start time (ns) -> the window's base file
    std::map<std::string, std::map<uint64_t, std::string>> start_time_file_name_map_;
    std::mutex start_time_file_name_map_mutex_;
    tl::managed<tl::xstream> archive_dir_monitoring_stream_;
    tl::managed<tl::thread> archive_dir_monitoring_thread_;

    // Feature flag and monitoring configuration
    bool use_polling_;
    std::chrono::milliseconds monitoring_interval_;
    // the grapher's story_chunk_duration_secs: the width of one archive file's
    // range, and so the step between the file names a probe tries. 0 disables
    // probing.
    uint64_t archive_window_secs_ = 30;
    // how far back from the end of a replay a probe reaches, in windows
    static constexpr uint64_t kProbeWindows = 4;
    std::chrono::system_clock::time_point last_scan_time_;

    // File system state tracking for polling
    std::map<std::string, FileInfo> previous_file_state_;
    std::mutex file_state_mutex_;

    // Directory caching for performance optimization
    std::map<fs::path, DirectoryCache> directory_cache_;
    std::mutex directory_cache_mutex_;

    // Thread control
    std::atomic<bool> shutdown_requested_;
    // set once the archive directory has been listed: until then a story
    // missing from the map means "not looked yet", not "nothing archived"
    std::atomic<bool> initial_scan_done_{false};
};

} // namespace chronolog

#endif //CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H
