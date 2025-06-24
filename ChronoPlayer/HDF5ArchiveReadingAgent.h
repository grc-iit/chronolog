#ifndef CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H
#define CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H

#include <list>
#include <string>
#include <map>
#include <filesystem>
#include <thallium.hpp>
#include <utility>

#include "StoryChunkIngestionQueue.h"

namespace tl = thallium;
namespace fs = std::filesystem;

namespace chronolog
{

class HDF5ArchiveReadingAgent
{
public:
    explicit HDF5ArchiveReadingAgent(std::string const &archive_path)
        : archive_path_(fs::absolute(expandTilde(fs::path(archive_path))).make_preferred().string())
    {}

    ~HDF5ArchiveReadingAgent() = default;

    int initialize()
    {
        LOG_INFO("[HDF5ArchiveReadingAgent] Initializing, scanning archive path {} recursively to create the map ..."
                 , archive_path_);
        createStartTimeFileNameMap();
        return setUpFsMonitoring();
    }

    int shutdown()
    {
        archive_dir_monitoring_stream_->join();
        archive_dir_monitoring_thread_->join();
        return 0;
    }

    int readArchivedStory(const ChronicleName&, const StoryName&, uint64_t, uint64_t, std::list<StoryChunk*>&);

    static std::string getChronicleName(const std::string &file_name)
    {
        // Example file name: /home/kfeng/chronolog/Debug/output/chronicle_0_0.story_0_0.1736806500.vlen.h5
        std::string base_name = fs::path(file_name).filename().string();
        std::string chronicle_name = base_name.substr(0, base_name.find_first_of('.'));
        return chronicle_name;
    }

    static std::string getStoryName(const std::string &file_name)
    {
        // Example file name: /home/kfeng/chronolog/Debug/output/chronicle_0_0.story_0_0.1736806500.vlen.h5
        std::string base_name = fs::path(file_name).filename().string();
        size_t first_dot = base_name.find_first_of('.');
        size_t second_dot = base_name.find_first_of('.', first_dot + 1);
        std::string story_name = base_name.substr(first_dot + 1, second_dot - first_dot - 1);
        return story_name;
    }

    static uint64_t getStartTime(const std::string &file_name)
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
        catch(const std::exception &e)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to convert start time string '{}' to uint64_t: {}"
                      , start_time_str, e.what());
        }
        return start_time_in_ns;
    }

private:
    fs::path expandTilde(fs::path path)
    {
        if(!path.empty() && path.string()[0] == '~')
        {
            const char *home = getenv("HOME");
            if(home)
            {
                fs::path expanded_path = fs::path(home) / path.string().substr(1);
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Expanding archive path from {} to {}"
                          , path.string(), expanded_path.string());
                return expanded_path;
            }
            else
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] HOME environment variable is not set. Cannot expand tilde in path: {}"
                          , path.string());
                return path; // Return the original path if HOME is not set
            }
        }
        return path;
    }

    bool isValidArchiveFile(const std::string &file_name)
    {
        fs::path expanded_full_path = fs::absolute(fs::path(file_name));
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Checking if file {} is a valid archive file.", expanded_full_path.string());
        fs::directory_entry entry(expanded_full_path);
        std::error_code ec;
        if(!fs::exists(entry, ec))
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] File {} does not exist. Skipping this file.", file_name);
            return false; // Skip files that do not exist
        }
        entry.refresh(ec);
        if(ec)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Error accessing file {}: {}", file_name, ec.message());
            return false; // Skip files that cannot be accessed
        }
        if(entry.is_regular_file(ec))
        {
            if(entry.path().extension() != ".h5")
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] File {} is not an HDF5 file. Skipping this file."
                          , entry.path().string());
                return false; // Skip non-HDF5 files
            }
        }
        else if(ec)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Error checking file {}: {}", entry.path().string(), ec.message());
            return false; // Skip files that cannot be accessed
        }
        else
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] File {} is not a regular file. Skipping this file.", entry.path().string());
            return false; // Skip non-regular files
        }
        std::ifstream file(entry.path());
        if(!file.is_open())
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to open file: {}. Skipping this file.", entry.path().string());
            return false; // Skip files that cannot be opened
        }
        return true;
    }

    int setUpFsMonitoring();

    void addRecursiveWatch(int inotify_fd, const std::string& path, std::map<int, std::string>& wd_to_path);

    int fsMonitoringThreadFunc();

    int createStartTimeFileNameMap()
    {
        // iterate over the HDF5 files in the archive directory to get the list of files
        // update the start_time_file_name_map_ with the start time and file name
        std::lock_guard <std::mutex> lock(start_time_file_name_map_mutex_);
        std::error_code ec;
        auto it = fs::recursive_directory_iterator(archive_path_, fs::directory_options::skip_permission_denied, ec);
        if(ec)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to iterate recursively over archive directory '{}': {}"
                      , archive_path_, ec.message());
            return -1; // Return error code on failure
        }
        for(const auto &entry: it)
        {
            if(ec)
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] Error accessing path: {}", ec.message());
                // Clear the error to continue iteration on other branches.
                ec.clear();
                continue;
            }
            if(!isValidArchiveFile(entry.path().string()))
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] Invalid archive file: {}. Skipping this file.", entry.path().string());
                continue; // Skip invalid files
            }
            std::string chronicle_name = getChronicleName(entry.path().string());
            std::string story_name = getStoryName(entry.path().string());
            uint64_t start_time = getStartTime(entry.path().string());
            if(start_time == 0)
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to extract start time from file: {}. Skipping this file."
                          , entry.path().string());
                continue; // Skip files with invalid start time
            }
            start_time_file_name_map_[std::make_tuple(chronicle_name, story_name, start_time)] = entry.path().string();
        }
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Created start_time_file_name_map_ with {} entries."
                  , start_time_file_name_map_.size());
        return 0;
    }

    int addFileToStartTimeFileNameMap(const std::string &file_name)
    {
        std::lock_guard<std::mutex> lock(start_time_file_name_map_mutex_);
        if(!isValidArchiveFile(file_name))
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Invalid archive file: {}. Skipping this file.", file_name);
            return -1; // Skip invalid files
        }
        std::string chronicle_name = getChronicleName(file_name);
        std::string story_name = getStoryName(file_name);
        uint64_t start_time = getStartTime(file_name);
        if(start_time == 0)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to extract start time from file: {}. Skipping this file."
                      , file_name);
            return -1; // Skip files with invalid start time
        }
        start_time_file_name_map_[std::make_tuple(chronicle_name, story_name, start_time)] = file_name;
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Added file {} to start_time_file_name_map_.", file_name);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] start_time_file_name_map_ has {} entries.",
                  start_time_file_name_map_.size());
        return 0;
    }

    int removeFileFromStartTimeFileNameMap(const std::string &file_name)
    {
        std::lock_guard<std::mutex> lock(start_time_file_name_map_mutex_);
        std::string chronicle_name = getChronicleName(file_name);
        std::string story_name = getStoryName(file_name);
        uint64_t start_time = getStartTime(file_name);
        if(start_time == 0)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to extract start time from file: {}. Skipping this file.",
                      file_name);
            return -1; // Skip files with invalid start time
        }
        start_time_file_name_map_.erase(std::make_tuple(chronicle_name, story_name, start_time));
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Removed file {} from start_time_file_name_map_.", file_name);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] start_time_file_name_map_ has {} entries.",
                  start_time_file_name_map_.size());
        return 0;
    }

    int renameFileInStartTimeFileNameMap(const std::string &old_file_name, const std::string &new_file_name)
    {
        removeFileFromStartTimeFileNameMap(old_file_name);
        addFileToStartTimeFileNameMap(new_file_name);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Renamed file {} to {} in start_time_file_name_map_.",
                  old_file_name, new_file_name);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] start_time_file_name_map_ has {} entries.",
                  start_time_file_name_map_.size());
        return 0;
    }

    std::string archive_path_;
    std::map<std::tuple<std::string, std::string, uint64_t>, std::string> start_time_file_name_map_;
    std::mutex start_time_file_name_map_mutex_;
    tl::managed <tl::xstream> archive_dir_monitoring_stream_;
    tl::managed <tl::thread> archive_dir_monitoring_thread_;
};

} // chronolog

#endif //CHRONOLOG_HDF5ARCHIVEREADINGAGENT_H
