#include <sys/inotify.h>
#include <H5Cpp.h>
#include <filesystem>

#include "chronolog_errcode.h"
#include "StoryChunkWriter.h"
#include "HDF5ArchiveReadingAgent.h"

namespace tl = thallium;

namespace chronolog
{
struct ErrorReport
{
    std::vector <std::string> messages;
};

herr_t error_walker(unsigned int n, const H5E_error2_t *err_desc, void *client_data)
{
    // Cast the client_data back to our ErrorReport struct
    auto *report = static_cast<ErrorReport *>(client_data);

    // Get the major and minor error strings
    char maj[256], min[256];
    H5Eget_msg(err_desc->maj_num, nullptr, maj, 256);
    H5Eget_msg(err_desc->min_num, nullptr, min, 256);

    // Format the detailed error message
    std::string msg =
            "HDF5 Error #" + std::to_string(n) + ":\n" + "  File: " + err_desc->file_name + "\n" + "  Line: " +
            std::to_string(err_desc->line) + "\n" + "  Function: " + err_desc->func_name + "\n" + "  Major Error: " +
            maj + "\n" + "  Minor Error: " + min + "\n" + "  Description: " + err_desc->desc;

    // Add the formatted message to our report
    report->messages.push_back(msg);

    // Return 0 to continue walking the stack
    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::readArchivedStory(const ChronicleName &chronicleName, const StoryName &storyName
                                                          , uint64_t startTime, uint64_t endTime
                                                          , std::list <StoryChunk *> &listOfChunks)
{
    // find all HDF5 files in the archive directory the start time of which falls in the range [startTime, endTime)
    // for each file, read Events in the StoryChunk and add matched ones to the list of StoryChunks
    // return the list of StoryChunks
    std::lock_guard <std::mutex> lock(start_time_file_name_map_mutex_);
    auto start_it = start_time_file_name_map_.lower_bound(std::make_tuple(chronicleName, storyName, startTime));
    auto end_it = start_time_file_name_map_.upper_bound(std::make_tuple(chronicleName, storyName, endTime));
    LOG_DEBUG("[HDF5ArchiveReadingAgent] readArchiveStory {}-{} range {}-{}", chronicleName, storyName, startTime
              , endTime);

    if(start_it == end_it)
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] No matching files found for story {}-{} in range {}-{}", chronicleName
                  , storyName, startTime, endTime);
        return CL_ERR_UNKNOWN;
    }

    LOG_DEBUG("[HDF5ArchiveReadingAgent] Found matching files for story {}-{} in range {}-{}", chronicleName, storyName
              , startTime, endTime);
    LOG_DEBUG(
            "[HDF5ArchiveReadingAgent] Start iterator: chronicle name: {}, story name: {}, start time: {}, file name: {}"
            , std::get <0>(start_it->first), std::get <1>(start_it->first), std::get <2>(start_it->first)
            , start_it->second);
    LOG_DEBUG(
            "[HDF5ArchiveReadingAgent] End iterator: chronicle name: {}, story name: {}, start time: {}, file name: {}"
            , std::get <0>(end_it->first), std::get <1>(end_it->first), std::get <2>(end_it->first), end_it->second);

    for(auto it = start_it; it != end_it; ++it)
    {
        fs::path file_full_path = fs::path(it->second);
        std::string file_name = file_full_path.string();
        std::unique_ptr <H5::H5File> file;
        StoryChunk *story_chunk = nullptr;
        try
        {
            H5::Exception::dontPrint();

            LOG_DEBUG("[HDF5ArchiveReadingAgent] Opening file: {}", file_name);
            file = std::make_unique <H5::H5File>(file_name, H5F_ACC_SWMR_READ);

            LOG_DEBUG("[HDF5ArchiveReadingAgent] Opening dataset {}", file_name);
            H5::DataSet dataset = file->openDataSet("/story_chunks/data.vlen_bytes");

            H5::DataSpace dataspace = dataset.getSpace();
            hsize_t dims_out[2] = {0, 0};
            dataspace.getSimpleExtentDims(dims_out, nullptr);
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading dataset {} with {} events", file_name, dims_out[0]);

            H5::CompType defined_comp_type = StoryChunkWriter::createEventCompoundType();
            H5::CompType probed_data_type = dataset.getCompType();
            if(probed_data_type.getNmembers() != defined_comp_type.getNmembers())
            {
                LOG_WARNING(
                        "[HDF5ArchiveReadingAgent] Error reading dataset {} : Not a compound type with the same #members"
                        , file_name);
                return CL_ERR_UNKNOWN;
            }
            if(probed_data_type != defined_comp_type)
            {
                LOG_WARNING("[HDF5ArchiveReadingAgent]Error reading dataset {} : Compound type mismatch", file_name);
                return CL_ERR_UNKNOWN;
            }

            LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading data from dataset {}", file_name);
            std::vector <LogEventHVL> data;
            data.resize(dims_out[0]);
            dataset.read(data.data(), defined_comp_type);

            LOG_DEBUG("[HDF5ArchiveReadingAgent] Creating StoryChunk {}-{} range {}-{}...", chronicleName, storyName
                      , startTime, endTime);
            story_chunk = new StoryChunk(chronicleName, storyName, 0, startTime, endTime);
            for(auto const &event_hvl: data)
            {
                if(event_hvl.eventTime < startTime)
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] Skipping event with time {} outside range {}-{}"
                              , event_hvl.eventTime, startTime, endTime);
                    continue;
                }
                if(event_hvl.eventTime >= endTime)
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] Stopping reading events at time {} outside range {}-{}"
                              , event_hvl.eventTime, startTime, endTime);
                    break;
                }

                LogEvent event(event_hvl.storyId, event_hvl.eventTime, event_hvl.clientId, event_hvl.eventIndex
                               , std::string(static_cast<char *>(event_hvl.logRecord.p), event_hvl.logRecord.len));
                story_chunk->insertEvent(event);
            }

            if(story_chunk->getEventCount() > 0)
            {
                listOfChunks.emplace_back(story_chunk);
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Inserted a StoryChunk with {} events {}-{} range {}-{} into list"
                          , story_chunk->getEventCount(), chronicleName, storyName, startTime, endTime);
            }
            else
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] No events in {}-{} are in range {}-{}, "
                          "no StoryChunk added to list", chronicleName, storyName, startTime, endTime);
                delete story_chunk;
            }
        }
        catch(H5::FileIException &error)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] reading file {} : FileIException: {} in C Function: {}", file_name
                      , error.getCDetailMsg(), error.getCFuncName());
            ErrorReport report;
            H5::Exception::walkErrorStack(H5E_WALK_UPWARD, error_walker, &report);
            for(const auto &msg: report.messages)
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] {}", msg);
            }

            delete story_chunk;
            return CL_ERR_UNKNOWN;
        }
        catch(H5::Exception &error)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] reading file {} : Exception: {} in C Function: {}", file_name
                      , error.getCDetailMsg(), error.getCFuncName());
            ErrorReport report;
            H5::Exception::walkErrorStack(H5E_WALK_UPWARD, error_walker, &report);
            for(const auto &msg: report.messages)
            {
                LOG_ERROR("[HDF5ArchiveReadingAgent] {}", msg);
            }
            delete story_chunk;
            return CL_ERR_UNKNOWN;
        }
    }

    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::setUpFsMonitoring()
{
    LOG_DEBUG("[HDF5ArchiveReadingAgent] Setting up file system monitoring for archive directory: '{}' recursively."
              , archive_path_);
    tl::managed <tl::xstream> es = tl::xstream::create();
    archive_dir_monitoring_stream_ = std::move(es);
    tl::managed <tl::thread> th = archive_dir_monitoring_stream_->make_thread([p = this]()
                                                                              { p->fsMonitoringThreadFunc(); });
    archive_dir_monitoring_thread_ = std::move(th);
    LOG_DEBUG("[HDF5ArchiveReadingAgent] Started archive directory monitoring thread.");
    return 0;
}

void chronolog::HDF5ArchiveReadingAgent::addRecursiveWatch(int inotify_fd, const std::string &path
                                                           , std::map <int, std::string> &wd_to_path)
{
    int wd = inotify_add_watch(inotify_fd, path.c_str(), IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO);
    if(wd < 0)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to add inotify watch on {}: {}", path, strerror(errno));
        return;
    }
    wd_to_path[wd] = fs::absolute(path).string();

    for(const auto &entry: fs::directory_iterator(path))
    {
        if(fs::is_directory(entry.status()))
        {
            addRecursiveWatch(inotify_fd, entry.path().string(), wd_to_path);
        }
    }
}

int chronolog::HDF5ArchiveReadingAgent::fsMonitoringThreadFunc()
{
    int inotifyFd = inotify_init();
    if(inotifyFd < 0)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to initialize inotify: {}", strerror(errno));
        return -1;
    }

    std::map <int, std::string> wd_to_path;
    addRecursiveWatch(inotifyFd, archive_path_, wd_to_path);

    const size_t eventSize = sizeof(struct inotify_event);
    const size_t bufLen = 1024 * (eventSize + 16);
    char buffer[bufLen];

    while(true)
    {
        ssize_t length = read(inotifyFd, buffer, bufLen);
        if(length < 0)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to read inotify events: {}", strerror(errno));
            break;
        }

        std::string old_file_name;
        for(ssize_t i = 0; i < length; i += (ssize_t)eventSize + ((struct inotify_event *)&buffer[i])->len)
        {
            auto *event = (struct inotify_event *)&buffer[i];
            std::string path;
            if(event->len)
            {
                path = (fs::path(wd_to_path[event->wd]) / event->name).string();
            }

            if(event->mask&(IN_CREATE))
            {
                if(event->mask&IN_ISDIR)
                {
                    // If a directory is created, we need to add a watch for it recursively
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] Directory {} created, updating inotify watch ..."
                              , path);
                    addRecursiveWatch(inotifyFd, fs::absolute(path), wd_to_path);
                }
                else
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] File {} created, updating file map...", path);
                    addFileToStartTimeFileNameMap(path);
                }
            }
            else if(event->mask&(IN_DELETE))
            {
                if(event->mask&IN_ISDIR)
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] Directory {} deleted, removing inotify watch ..."
                              , path);
                    // Remove the watch for the deleted directory
                    wd_to_path.erase(event->wd);
                }
                else
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] File {} deleted, updating file map...", path);
                    removeFileFromStartTimeFileNameMap(path);
                }
            }
            else if(event->mask&(IN_MOVED_FROM))
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] File is renamed from {}", path);
                old_file_name = path;
            }
            else if(event->mask&(IN_MOVED_TO))
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

    for(const auto &entry: wd_to_path)
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Removing inotify watch for {} with wd {}", entry.second, entry.first);
        inotify_rm_watch(inotifyFd, entry.first);
    }
    close(inotifyFd);

    return 0;
}
} // chronolog
