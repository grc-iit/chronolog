#include <sys/inotify.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <fcntl.h>
#include <H5Cpp.h>
#include <filesystem>
#include <thread>
#include <chrono>

#include <chronolog_errcode.h>
#include <StoryChunkWriter.h>
#include <HDF5ArchiveReadingAgent.h>

namespace tl = thallium;

// Helper function to format uint64_t with comma separators
std::string formatWithCommas(uint64_t value)
{
    std::string str = std::to_string(value);
    int pos = str.length() - 3;
    while(pos > 0)
    {
        str.insert(pos, ",");
        pos -= 3;
    }
    return str;
}

// Helper function to convert filesystem::file_time_type to system_clock nanoseconds
int64_t convertFileTimeToSystemClockNs(const fs::file_time_type& file_time)
{
    // Get current system time for reference
    auto now_sys = std::chrono::system_clock::now();
    auto now_file = std::filesystem::file_time_type::clock::now();

    // Calculate the offset between file_time_type and system_clock
    auto file_duration = file_time.time_since_epoch();
    auto file_now_duration = now_file.time_since_epoch();
    auto sys_duration = now_sys.time_since_epoch();

    // Convert to system_clock time_point
    auto offset = sys_duration - file_now_duration;
    auto sys_time = std::chrono::system_clock::time_point(file_duration + offset);

    return std::chrono::duration_cast<std::chrono::nanoseconds>(sys_time.time_since_epoch()).count();
}

namespace chronolog
{
struct ErrorReport
{
    std::vector<std::string> messages;
};

herr_t error_walker(unsigned int n, const H5E_error2_t* err_desc, void* client_data)
{
    // Cast the client_data back to our ErrorReport struct
    auto* report = static_cast<ErrorReport*>(client_data);

    // Get the major and minor error strings
    char maj[256], min[256];
    H5Eget_msg(err_desc->maj_num, nullptr, maj, 256);
    H5Eget_msg(err_desc->min_num, nullptr, min, 256);

    // Format the detailed error message
    std::string msg = "HDF5 Error #" + std::to_string(n) + ":\n" + "  File: " + err_desc->file_name + "\n" +
                      "  Line: " + std::to_string(err_desc->line) + "\n" + "  Function: " + err_desc->func_name + "\n" +
                      "  Major Error: " + maj + "\n" + "  Minor Error: " + min + "\n" +
                      "  Description: " + err_desc->desc;

    // Add the formatted message to our report
    report->messages.push_back(msg);

    // Return 0 to continue walking the stack
    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::readStoryChunkFile(const ChronicleName& chronicleName,
                                                           const StoryName& storyName,
                                                           uint64_t startTime,
                                                           uint64_t endTime,
                                                           std::list<StoryChunk*>& listOfChunks,
                                                           const std::string& file_name)
{
    std::unique_ptr<H5::H5File> file;
    StoryChunk* story_chunk = nullptr;
    try
    {
        H5::Exception::dontPrint();

        LOG_DEBUG("[HDF5ArchiveReadingAgent] Opening file {}", file_name);
        file = std::make_unique<H5::H5File>(file_name, H5F_ACC_SWMR_READ);

        std::string dataset_name = "/story_chunks/data.vlen_bytes";
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Opening dataset {}", dataset_name);
        H5::DataSet dataset = file->openDataSet(dataset_name);

        H5::DataSpace dataspace = dataset.getSpace();
        hsize_t dims_out[2] = {0, 0};
        dataspace.getSimpleExtentDims(dims_out, nullptr);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading dataset {} with {} events",
                  dataset_name,
                  formatWithCommas(dims_out[0]));

        H5::CompType defined_comp_type = StoryChunkWriter::createEventCompoundType();
        H5::CompType probed_data_type = dataset.getCompType();
        if(probed_data_type.getNmembers() != defined_comp_type.getNmembers())
        {
            LOG_WARNING(
                    "[HDF5ArchiveReadingAgent] Error reading dataset {} : Not a compound type with the same #members",
                    file_name);
            return CL_ERR_UNKNOWN;
        }
        if(probed_data_type != defined_comp_type)
        {
            LOG_WARNING("[HDF5ArchiveReadingAgent]Error reading dataset {} : Compound type mismatch", file_name);
            return CL_ERR_UNKNOWN;
        }

        LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading data from dataset {}", dataset_name);
        std::vector<LogEventHVL> data;
        data.resize(dims_out[0]);
        dataset.read(data.data(), defined_comp_type);

        LOG_DEBUG("[HDF5ArchiveReadingAgent] Creating StoryChunk {}-{} range {}-{}...",
                  chronicleName,
                  storyName,
                  formatWithCommas(startTime),
                  formatWithCommas(endTime));
        story_chunk = new StoryChunk(chronicleName, storyName, 0, startTime, endTime);
        for(auto const& event_hvl: data)
        {
            if(event_hvl.eventTime < startTime)
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Skipping event with time {} outside range {}-{}",
                          formatWithCommas(event_hvl.eventTime),
                          formatWithCommas(startTime),
                          formatWithCommas(endTime));
                continue;
            }
            if(event_hvl.eventTime >= endTime)
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Stopping reading events at time {} outside range {}-{}",
                          formatWithCommas(event_hvl.eventTime),
                          formatWithCommas(startTime),
                          formatWithCommas(endTime));
                break;
            }

            LogEvent event(event_hvl.storyId,
                           event_hvl.eventTime,
                           event_hvl.clientId,
                           event_hvl.eventIndex,
                           std::string(static_cast<char*>(event_hvl.logRecord.p), event_hvl.logRecord.len));
            story_chunk->insertEvent(event);
        }

        if(story_chunk->getEventCount() > 0)
        {
            listOfChunks.emplace_back(story_chunk);
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Inserted a StoryChunk with {} events {}-{} range {}-{} into list",
                      formatWithCommas(story_chunk->getEventCount()),
                      chronicleName,
                      storyName,
                      formatWithCommas(startTime),
                      formatWithCommas(endTime));
        }
        else
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] No events in {}-{} are in range {}-{}, "
                      "no StoryChunk added to list",
                      chronicleName,
                      storyName,
                      formatWithCommas(startTime),
                      formatWithCommas(endTime));
            delete story_chunk;
        }
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] reading file {} : FileIException: {} in C Function: {}",
                  file_name,
                  error.getCDetailMsg(),
                  error.getCFuncName());
        ErrorReport report;
        H5::Exception::walkErrorStack(H5E_WALK_UPWARD, error_walker, &report);
        for(const auto& msg: report.messages) { LOG_ERROR("[HDF5ArchiveReadingAgent] {}", msg); }

        delete story_chunk;
        return -1;
    }
    catch(H5::Exception& error)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] reading file {} : Exception: {} in C Function: {}",
                  file_name,
                  error.getCDetailMsg(),
                  error.getCFuncName());
        ErrorReport report;
        H5::Exception::walkErrorStack(H5E_WALK_UPWARD, error_walker, &report);
        for(const auto& msg: report.messages) { LOG_ERROR("[HDF5ArchiveReadingAgent] {}", msg); }
        delete story_chunk;
        return -1;
    }
    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::readArchivedStory(const ChronicleName& chronicleName,
                                                          const StoryName& storyName,
                                                          uint64_t startTime,
                                                          uint64_t endTime,
                                                          std::list<StoryChunk*>& listOfChunks,
                                                          bool readAuxFiles)
{
    // find all HDF5 files in the archive directory the start time of which falls in the range [startTime, endTime)
    // for each file, read Events in the StoryChunk and add matched ones to the list of StoryChunks
    // return the list of StoryChunks
    std::lock_guard<std::mutex> lock(start_time_file_name_map_mutex_);
    if(!readAuxFiles)
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading archived story {}-{} range {}-{}, main file only",
                  chronicleName,
                  storyName,
                  formatWithCommas(startTime),
                  formatWithCommas(endTime));
    }
    else
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading archived story {}-{} range {}-{}, main and auxiliary files",
                  chronicleName,
                  storyName,
                  formatWithCommas(startTime),
                  formatWithCommas(endTime));
    }
    // Find the last file whose start time <= startTime for the SAME chronicle-story combination
    auto start_it = start_time_file_name_map_.upper_bound(std::make_tuple(chronicleName, storyName, startTime));
    if(start_it != start_time_file_name_map_.begin())
    {
        --start_it;
        // Check if the decremented iterator still belongs to the same chronicle-story combination
        if(std::get<0>(start_it->first) != chronicleName || std::get<1>(start_it->first) != storyName)
        {
            // The previous entry belongs to a different chronicle-story combination
            ++start_it;
        }
    }

    // Find first file whose start time > endTime (this part you already have correct)
    auto end_it = start_time_file_name_map_.upper_bound(std::make_tuple(chronicleName, storyName, endTime));
    LOG_DEBUG("[HDF5ArchiveReadingAgent] readArchiveStory {}-{} range {}-{}",
              chronicleName,
              storyName,
              formatWithCommas(startTime),
              formatWithCommas(endTime));

    if(start_it == end_it)
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] No matching files found for story {}-{} in range {}-{}",
                  chronicleName,
                  storyName,
                  formatWithCommas(startTime),
                  formatWithCommas(endTime));
        return CL_ERR_UNKNOWN;
    }

    LOG_DEBUG("[HDF5ArchiveReadingAgent] Found matching files for story {}-{} in range {}-{}",
              chronicleName,
              storyName,
              formatWithCommas(startTime),
              formatWithCommas(endTime));
    LOG_DEBUG("[HDF5ArchiveReadingAgent] Start iterator: chronicle name: {}, story name: {}, start time: {}, file "
              "name: {}",
              std::get<0>(start_it->first),
              std::get<1>(start_it->first),
              formatWithCommas(std::get<2>(start_it->first)),
              start_it->second);
    if(end_it != start_time_file_name_map_.end())
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] End iterator: chronicle name: {}, story name: {}, start time: {}, file "
                  "name: {}",
                  std::get<0>(end_it->first),
                  std::get<1>(end_it->first),
                  formatWithCommas(std::get<2>(end_it->first)),
                  end_it->second);
    }
    else
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] End iterator: end of map");
    }

    fs::path file_full_path;
    std::string file_name, next_file_name, next_file_number_str;

    for(auto it = start_it; it != end_it; ++it)
    {
        file_full_path = fs::path(it->second);

        // file_name should be in the format of /path/to/output/{chronicleName}.{storyName}.{startTime}.vlen.h5
        file_name = file_full_path.string();
        readStoryChunkFile(chronicleName, storyName, startTime, endTime, listOfChunks, file_name);

        if(readAuxFiles)
        {
            // next_file_name should be in the format of {chronicleName}.{storyName}.{startTime}.vlen.{number}.h5
            next_file_name = StoryChunkWriter::getStoryChunkFileName(archive_path_, file_full_path.filename().string());
            next_file_number_str = fs::path(next_file_name).replace_extension("").extension().string().substr(1);
            if(next_file_number_str == "vlen")
            {
                // should not happen, but just in case
                LOG_ERROR("[HDF5ArchiveReadingAgent] getStoryChunkFileName returned a file name without a number: {},"
                          " indicating main story chunk file {} does not exist. ",
                          next_file_name,
                          file_name);
                return -1;
            }
            else if(std::all_of(next_file_number_str.begin(), next_file_number_str.end(), ::isdigit))
            {
                // next_file_name is a numbered file, then read from .1 to .next_file_number_str-1
                for(int i = 1; i < std::stoi(next_file_number_str); ++i)
                {
                    file_name = file_full_path.parent_path() / fs::path(file_full_path).replace_extension("").string();
                    file_name += "." + std::to_string(i) + ".h5";
                    // file_name should be /path/to/output/chronicleName.storyName.startTime.vlen.{i}.h5 now
                    if(fs::exists(file_name))
                    {
                        LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading numbered file: {}", file_name);
                        readStoryChunkFile(chronicleName, storyName, startTime, endTime, listOfChunks, file_name);
                    }
                    else
                    {
                        LOG_DEBUG("[HDF5ArchiveReadingAgent] Numbered file {} does not exist, skipping.", file_name);
                        continue; // Skip if the numbered file does not exist
                    }
                }
            }
            else
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] Something went wrong with file name: {}.", file_name);
            }
        }
    }

    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::setUpFsMonitoring()
{
    if(use_polling_)
    {
        LOG_DEBUG(
                "[HDF5ArchiveReadingAgent] Setting up polling-based file system monitoring for archive directory: '{}'",
                archive_path_);
    }
    else
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Setting up inotify-based file system monitoring for archive directory: "
                  "'{}' recursively.",
                  archive_path_);
    }

    tl::managed<tl::xstream> es = tl::xstream::create();
    archive_dir_monitoring_stream_ = std::move(es);

    if(use_polling_)
    {
        tl::managed<tl::thread> th =
                archive_dir_monitoring_stream_->make_thread([p = this]() { p->pollingMonitoringThreadFunc(); });
        archive_dir_monitoring_thread_ = std::move(th);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Started polling-based archive directory monitoring thread.");
    }
    else
    {
        tl::managed<tl::thread> th =
                archive_dir_monitoring_stream_->make_thread([p = this]() { p->inotifyMonitoringThreadFunc(); });
        archive_dir_monitoring_thread_ = std::move(th);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Started inotify-based archive directory monitoring thread.");
    }

    return 0;
}

void chronolog::HDF5ArchiveReadingAgent::addRecursiveWatch(int inotify_fd,
                                                           const std::string& path,
                                                           std::map<int, std::string>& wd_to_path)
{
    int wd = inotify_add_watch(inotify_fd, path.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if(wd < 0)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to add inotify watch on {}: {}", path, strerror(errno));
        return;
    }
    wd_to_path[wd] = fs::absolute(path).string();

    for(const auto& entry: fs::directory_iterator(path))
    {
        if(fs::is_directory(entry.status()))
        {
            addRecursiveWatch(inotify_fd, entry.path().string(), wd_to_path);
        }
    }
}

int chronolog::HDF5ArchiveReadingAgent::inotifyMonitoringThreadFunc()
{
    int inotifyFd = inotify_init();
    if(inotifyFd < 0)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to initialize inotify: {}", strerror(errno));
        return -1;
    }

    // Make the inotify file descriptor non-blocking
    int flags = fcntl(inotifyFd, F_GETFL, 0);
    if(flags < 0)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to get inotify fd flags: {}", strerror(errno));
        close(inotifyFd);
        return -1;
    }
    if(fcntl(inotifyFd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to set inotify fd non-blocking: {}", strerror(errno));
        close(inotifyFd);
        return -1;
    }

    std::map<int, std::string> wd_to_path;
    addRecursiveWatch(inotifyFd, archive_path_, wd_to_path);

    const size_t eventSize = sizeof(struct inotify_event);
    const size_t bufLen = 1024 * (eventSize + 16);
    char buffer[bufLen];

    while(!shutdown_requested_.load())
    {
        ssize_t length = read(inotifyFd, buffer, bufLen);
        if(length < 0)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // No data available, sleep for monitoring interval and check shutdown flag
                std::this_thread::sleep_for(monitoring_interval_);
                continue;
            }
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to read inotify events: {}", strerror(errno));
            break;
        }

        if(length == 0)
        {
            // No data available, sleep for monitoring interval and check shutdown flag
            std::this_thread::sleep_for(monitoring_interval_);
            continue;
        }

        std::string old_file_name;
        for(ssize_t i = 0; i < length; i += (ssize_t)eventSize + ((struct inotify_event*)&buffer[i])->len)
        {
            auto* event = (struct inotify_event*)&buffer[i];
            std::string path;
            if(event->len)
            {
                path = (fs::path(wd_to_path[event->wd]) / event->name).string();
            }

            if(event->mask & (IN_CREATE))
            {
                if(event->mask & IN_ISDIR)
                {
                    // If a directory is created, we need to add a watch for it recursively
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] Directory {} created, updating inotify watch ...", path);
                    addRecursiveWatch(inotifyFd, fs::absolute(path), wd_to_path);
                }
                else
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] File {} created, updating file map...", path);
                    addFileToStartTimeFileNameMap(path);
                }
            }
            else if(event->mask & (IN_DELETE))
            {
                if(event->mask & IN_ISDIR)
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] Directory {} deleted, removing inotify watch ...", path);
                    // Remove the watch for the deleted directory
                    wd_to_path.erase(event->wd);
                }
                else
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] File {} deleted, updating file map...", path);
                    removeFileFromStartTimeFileNameMap(path);
                }
            }
            else if(event->mask & (IN_MOVED_FROM))
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] File is renamed from {}", path);
                old_file_name = path;
            }
            else if(event->mask & (IN_MOVED_TO))
            {
                if(old_file_name.empty())
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] File {} created, updating file map...", path);
                    addFileToStartTimeFileNameMap(path);
                }
                else
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] File is renamed to {}, updating file map...", path);
                    std::string new_file_name = path;
                    renameFileInStartTimeFileNameMap(old_file_name, new_file_name);
                    old_file_name.clear();
                }
            }
        }
    }

    for(const auto& entry: wd_to_path)
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Removing inotify watch for {} with wd {}", entry.second, entry.first);
        inotify_rm_watch(inotifyFd, entry.first);
    }
    close(inotifyFd);

    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::pollingMonitoringThreadFunc()
{
    LOG_DEBUG("[HDF5ArchiveReadingAgent] Starting polling-based file system monitoring for archive directory: '{}'",
              archive_path_);

    // Set the initial scan time BEFORE the initial scan
    last_scan_time_ = std::chrono::system_clock::now();

    // Initial scan to establish baseline
    scanFileSystem();

    while(!shutdown_requested_.load())
    {
        std::this_thread::sleep_for(monitoring_interval_);

        if(shutdown_requested_.load())
        {
            break;
        }

        // Always scan the file system to detect deletions, modifications, and additions
        // The hasFileSystemChanged() method cannot reliably detect file deletions since
        // directory iteration only shows existing files
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Performing periodic file system scan...");
        updateFileState();
        scanFileSystem();
    }

    return 0;
}

void chronolog::HDF5ArchiveReadingAgent::scanFileSystem()
{
    // Enhanced polling mode that detects file creation, deletion, modification, and renaming
    // Rename detection works by matching disappeared files with new files based on:
    // - Same file size
    // - Same modification time
    // - Both are regular files (not directories)
    std::vector<FileInfo> current_state = getCurrentFileState();
    std::lock_guard<std::mutex> lock(file_state_mutex_);

    // Track new and missing files for rename detection
    std::vector<FileInfo> new_files;
    std::vector<std::pair<std::string, FileInfo>> missing_files;

    // Compare with previous state and update file map
    for(const auto& file_info: current_state)
    {
        auto it = previous_file_state_.find(file_info.path);
        if(it == previous_file_state_.end())
        {
            // Potentially new file (could be renamed)
            new_files.push_back(file_info);
        }
        else if(it->second.last_modified != file_info.last_modified)
        {
            // File modified - for HDF5 files, this might indicate new data
            LOG_DEBUG("[HDF5ArchiveReadingAgent] File modified: {}", file_info.path);
            // Note: We don't need to update the file map for modifications since the file path hasn't changed
        }
    }

    // Check for deleted/missing files
    for(const auto& prev_file: previous_file_state_)
    {
        bool still_exists = false;
        for(const auto& curr_file: current_state)
        {
            if(curr_file.path == prev_file.first)
            {
                still_exists = true;
                break;
            }
        }
        if(!still_exists)
        {
            // Potentially deleted file (could be renamed)
            missing_files.emplace_back(prev_file.first, prev_file.second);
            LOG_DEBUG("[HDF5ArchiveReadingAgent] File missing: {}", prev_file.first);
        }
    }

    // Detect renames by matching missing files with new files based on size and modification time
    for(auto missing_it = missing_files.begin(); missing_it != missing_files.end();)
    {
        bool found_rename = false;
        const auto& missing_file = missing_it->second;

        for(auto new_it = new_files.begin(); new_it != new_files.end(); ++new_it)
        {
            const auto& new_file = *new_it;

            // Match files by size, modification time, and ensure both are regular files
            if(!missing_file.is_directory && !new_file.is_directory && missing_file.file_size == new_file.file_size &&
               missing_file.last_modified == new_file.last_modified)
            {
                // This looks like a rename!
                LOG_DEBUG("[HDF5ArchiveReadingAgent] File renamed from {} to {}", missing_it->first, new_file.path);
                renameFileInStartTimeFileNameMap(missing_it->first, new_file.path);

                // Remove from both lists since we handled the rename
                missing_it = missing_files.erase(missing_it);
                new_files.erase(new_it);
                found_rename = true;
                break;
            }
        }

        if(!found_rename)
        {
            ++missing_it;
        }
    }

    // Handle remaining new files (actual creations)
    for(const auto& file_info: new_files)
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] New file detected: {}", file_info.path);
        addFileToStartTimeFileNameMap(file_info.path);
    }

    // Handle remaining missing files (actual deletions)
    for(const auto& missing_file: missing_files)
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] File deleted: {}", missing_file.first);
        removeFileFromStartTimeFileNameMap(missing_file.first);
    }

    // Update previous state
    previous_file_state_.clear();
    for(const auto& file_info: current_state) { previous_file_state_[file_info.path] = file_info; }
}

bool chronolog::HDF5ArchiveReadingAgent::hasFileSystemChanged()
{
    std::error_code ec;
    // Use system_clock for consistent time comparison
    auto last_scan_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(last_scan_time_.time_since_epoch()).count();

    LOG_DEBUG("[HDF5ArchiveReadingAgent] Checking file system changes, last_scan_ns: {}",
              formatWithCommas(last_scan_ns));

    // Use cached directory checking for better performance
    return hasDirectoryChangedWithCache(archive_path_, last_scan_ns, ec);
}

bool chronolog::HDF5ArchiveReadingAgent::hasDirectoryChangedOptimizedRecursive(const fs::path& dir_path,
                                                                               int64_t last_scan_ns,
                                                                               std::error_code& ec)
{
    // Check if this directory has been modified
    auto dir_last_write = fs::last_write_time(dir_path, ec);
    if(!ec)
    {
        auto dir_last_write_ns = convertFileTimeToSystemClockNs(dir_last_write);

        // If directory hasn't been modified, skip this entire subtree
        if(dir_last_write_ns <= last_scan_ns)
        {
            return false;
        }
    }
    else
    {
        LOG_WARNING("[HDF5ArchiveReadingAgent] Error getting directory modification time for {}: {}",
                    dir_path.string(),
                    ec.message());
        ec.clear();
    }

    // Directory was modified, check its contents
    for(const auto& entry: fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec))
    {
        if(ec)
        {
            LOG_WARNING("[HDF5ArchiveReadingAgent] Error accessing path during change detection: {}", ec.message());
            ec.clear();
            continue;
        }

        if(entry.is_directory())
        {
            // Recursively check subdirectories - return immediately if any change found
            if(hasDirectoryChangedOptimizedRecursive(entry.path(), last_scan_ns, ec))
            {
                return true;
            }
        }
        else
        {
            // For all files, check if they're valid HDF5 archive files and if they've been modified
            if(isValidArchiveFile(entry.path().string()))
            {
                auto file_last_write = fs::last_write_time(entry.path(), ec);
                if(!ec)
                {
                    auto file_last_write_ns = convertFileTimeToSystemClockNs(file_last_write);

                    if(file_last_write_ns > last_scan_ns)
                    {
                        LOG_DEBUG("[HDF5ArchiveReadingAgent] Detected modified HDF5 file: {}", entry.path().string());
                        return true;
                    }
                }
                else
                {
                    LOG_WARNING("[HDF5ArchiveReadingAgent] Error getting file modification time for {}: {}",
                                entry.path().string(),
                                ec.message());
                    ec.clear();
                }
            }
        }
    }

    return false;
}

// Directory caching implementation
bool chronolog::HDF5ArchiveReadingAgent::hasDirectoryChangedWithCache(const fs::path& dir_path,
                                                                      int64_t last_scan_ns,
                                                                      std::error_code& ec)
{
    // Get current directory modification time
    int64_t current_mod_time = getDirectoryModificationTime(dir_path, ec);
    if(ec)
    {
        LOG_WARNING("[HDF5ArchiveReadingAgent] Error getting directory modification time for {}: {}",
                    dir_path.string(),
                    ec.message());
        ec.clear();
        // Continue with actual check if we can't get modification time
    }

    // Check cache first
    {
        std::lock_guard<std::mutex> lock(directory_cache_mutex_);
        auto it = directory_cache_.find(dir_path);
        if(it != directory_cache_.end())
        {
            // Only use cached "no changes" result if:
            // 1. Directory hasn't been modified since last check, AND
            // 2. We checked it after the last scan time
            if(current_mod_time <= it->second.last_modified_ns && it->second.last_check_time_ns > last_scan_ns)
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Using cached result for directory {}: no changes detected",
                          dir_path.string());
                return it->second.has_changes;
            }
        }
    }

    // Perform the actual check
    bool has_changes = hasDirectoryChangedOptimizedRecursive(dir_path, last_scan_ns, ec);

    // Update cache with current modification time
    updateDirectoryCache(dir_path, current_mod_time, last_scan_ns, has_changes);

    return has_changes;
}

void chronolog::HDF5ArchiveReadingAgent::updateDirectoryCache(const fs::path& dir_path,
                                                              int64_t last_modified_ns,
                                                              int64_t check_time_ns,
                                                              bool has_changes)
{
    std::lock_guard<std::mutex> lock(directory_cache_mutex_);
    directory_cache_[dir_path] = DirectoryCache(dir_path, last_modified_ns, check_time_ns, has_changes);

    LOG_DEBUG("[HDF5ArchiveReadingAgent] Updated directory cache for {}: modified_ns={}, check_ns={}, has_changes={}",
              dir_path.string(),
              formatWithCommas(last_modified_ns),
              formatWithCommas(check_time_ns),
              has_changes);
}

void chronolog::HDF5ArchiveReadingAgent::clearDirectoryCache()
{
    std::lock_guard<std::mutex> lock(directory_cache_mutex_);
    directory_cache_.clear();
    LOG_DEBUG("[HDF5ArchiveReadingAgent] Cleared directory cache");
}

int64_t chronolog::HDF5ArchiveReadingAgent::getDirectoryModificationTime(const fs::path& dir_path, std::error_code& ec)
{
    auto dir_last_write = fs::last_write_time(dir_path, ec);
    if(!ec)
    {
        return convertFileTimeToSystemClockNs(dir_last_write);
    }
    LOG_WARNING("[HDF5ArchiveReadingAgent] Error getting directory modification time for {}: {}",
                dir_path.string(),
                ec.message());
    ec.clear();
    return 0;
}

void chronolog::HDF5ArchiveReadingAgent::updateFileState() { last_scan_time_ = std::chrono::system_clock::now(); }

std::vector<chronolog::HDF5ArchiveReadingAgent::FileInfo> chronolog::HDF5ArchiveReadingAgent::getCurrentFileState()
{
    std::vector<FileInfo> current_state;
    std::error_code ec;

    for(const auto& entry:
        fs::recursive_directory_iterator(archive_path_, fs::directory_options::skip_permission_denied, ec))
    {
        if(ec)
        {
            LOG_WARNING("[HDF5ArchiveReadingAgent] Error accessing path during file state scan: {}", ec.message());
            ec.clear();
            continue;
        }

        try
        {
            std::string path = entry.path().string();
            bool is_dir = entry.is_directory(ec);
            if(ec)
            {
                LOG_WARNING("[HDF5ArchiveReadingAgent] Error checking if path is directory: {}", ec.message());
                ec.clear();
                continue;
            }

            if(is_dir)
            {
                // For directories, we only need basic info
                current_state.emplace_back(path, fs::file_time_type{}, 0, true);
            }
            else
            {
                // For files, get detailed info
                auto last_write = fs::last_write_time(entry.path(), ec);
                if(ec)
                {
                    LOG_WARNING("[HDF5ArchiveReadingAgent] Error getting last write time for {}: {}",
                                path,
                                ec.message());
                    ec.clear();
                    continue;
                }

                auto file_size = entry.file_size(ec);
                if(ec)
                {
                    LOG_WARNING("[HDF5ArchiveReadingAgent] Error getting file size for {}: {}", path, ec.message());
                    ec.clear();
                    file_size = 0;
                }

                current_state.emplace_back(path, last_write, file_size, false);
            }
        }
        catch(const std::exception& e)
        {
            LOG_WARNING("[HDF5ArchiveReadingAgent] Exception during file state scan: {}", e.what());
            continue;
        }
    }

    return current_state;
}

} // namespace chronolog
