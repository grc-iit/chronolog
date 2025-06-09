#include <sys/inotify.h>
#include <H5Cpp.h>

#include "chronolog_errcode.h"
#include "StoryChunkWriter.h"
#include "HDF5ArchiveReadingAgent.h"

namespace tl = thallium;

namespace chronolog
{
int chronolog::HDF5ArchiveReadingAgent::readArchivedStory(const ChronicleName &chronicleName, const StoryName &storyName
                                               , uint64_t startTime, uint64_t endTime
                                               , std::list <StoryChunk *> &listOfChunks)
{
    // find all HDF5 files in the archive directory the start time of which falls in the range [startTime, endTime)
    // for each file, read Events in the StoryChunk and add matched ones to the list of StoryChunks
    // return the list of StoryChunks
    std::lock_guard <std::mutex> lock(start_time_file_name_map_mutex_);
    auto start_it = start_time_file_name_map_.lower_bound(std::make_tuple(chronicleName, storyName
                                                                          , startTime / 1000000000));
    auto end_it = start_time_file_name_map_.upper_bound(std::make_tuple(chronicleName, storyName
                                                                        , endTime / 1000000000));
    LOG_DEBUG("[HDF5ArchiveReadingAgent] readArchiveStory {}-{} range{}-{}", chronicleName,storyName, startTime,endTime);


    if(start_it == start_time_file_name_map_.end())
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] No matching files found for story {}-{} in range {}-{}" , chronicleName, storyName, startTime,endTime);
        return CL_ERR_UNKNOWN;
    }

    LOG_DEBUG("[HDF5ArchiveReadingAgent] Found  matching files for story {}-{} in range {}-{}", chronicleName, storyName, startTime,endTime);
    do
    {
        std::string file_name = start_it->second;
        std::unique_ptr <H5::H5File> file;
        try
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Opening file: {}", file_name);
            file = std::make_unique <H5::H5File>(file_name, H5F_ACC_SWMR_READ);

            LOG_DEBUG("[HDF5ArchiveReadingAgent] Opening dataset {}", file_name);
            H5::DataSet dataset = file->openDataSet("/story_chunks/data.vlen_bytes");

            H5::DataSpace dataspace = dataset.getSpace();
            hsize_t dims_out[2] = {0, 0};
            dataspace.getSimpleExtentDims(dims_out, nullptr);
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading dataset {} with {} events", file_name,dims_out[0]);

            H5::CompType defined_comp_type = StoryChunkWriter::createEventCompoundType();
            H5::CompType probed_data_type = dataset.getCompType();
            if(probed_data_type.getNmembers() != defined_comp_type.getNmembers())
            {
                LOG_WARNING("[HDF5ArchiveReadingAgent] Error reading dataset {} : Not a compound type with the same #members", file_name);
                return CL_ERR_UNKNOWN;
            }
            if(probed_data_type != defined_comp_type)
            {
                LOG_WARNING("[HDF5ArchiveReadingAgent]Error reading dataset {} : Compound type mismatch", file_name);
                return CL_ERR_UNKNOWN;
            }

            LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading data from dataset {}",file_name);
            std::vector <LogEventHVL> data;
            data.resize(dims_out[0]);
            dataset.read(data.data(), defined_comp_type);

            LOG_DEBUG("[HDF5ArchiveReadingAgent] Creating StoryChunk {}-{} range {}-{}...", chronicleName,storyName, startTime, endTime);
            auto *story_chunk = new StoryChunk(chronicleName, storyName, 0, startTime, endTime);
            uint64_t event_count = 0;
            for(auto const &event_hvl: data)
            {
                if(event_hvl.eventTime < startTime || event_hvl.eventTime > endTime)
                {
                    continue;
                }

                LogEvent event(event_hvl.storyId, event_hvl.eventTime
                               , event_hvl.clientId, event_hvl.eventIndex
                               , std::string(static_cast<char *>(event_hvl.logRecord.p), event_hvl.logRecord.len));
                story_chunk->insertEvent(event);
                event_count++;
            }
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Inserted {} events into StoryChunk {}-{} range {}-{}"
                      , event_count, chronicleName, storyName,startTime, endTime);

            listOfChunks.emplace_back(story_chunk);

//            dataset.close();
//            dataspace.close();
//            file->close();
        }
        catch(H5::FileIException &error)
        {
            LOG_ERROR("[HDF5ArchiveReadingAgent] reading file{} : FileIException: {}", file_name, error.getCDetailMsg());
// INNA:  there's potential memory leak here ... need to cleanup the memory allocated for the StoryChunk.. 
#ifdef DEBUG
            H5::FileIException::printErrorStack();
#endif
            return CL_ERR_UNKNOWN;
        }
    } while(++start_it != end_it);

    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::setUpFsMonitoring()
{
    LOG_DEBUG("[HDF5ArchiveReadingAgent] Setting up file system monitoring for archive directory: '{}'."
              , archive_path_);
    tl::managed <tl::xstream> es = tl::xstream::create();
    archive_dir_monitoring_stream_ = std::move(es);
    tl::managed <tl::thread> th = archive_dir_monitoring_stream_->make_thread([p = this]()
                                                                              { p->fsMonitoringThreadFunc(); });
    archive_dir_monitoring_thread_ = std::move(th);
    LOG_DEBUG("[HDF5ArchiveReadingAgent] Started archive directory monitoring thread.");
    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::fsMonitoringThreadFunc()
{
    int inotifyFd = inotify_init();
    if(inotifyFd < 0)
    {
        perror("inotify_init");
        return -1;
    }

    int watchFd = inotify_add_watch(inotifyFd, archive_path_.c_str(), IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO);
    if(watchFd < 0)
    {
        perror("inotify_add_watch");
        close(inotifyFd);
        return -1;
    }

    const size_t eventSize = sizeof(struct inotify_event);
    const size_t bufLen = 1024 * (eventSize + 16);
    char buffer[bufLen];

    while(true)
    {
        int length = read(inotifyFd, buffer, bufLen);
        if(length < 0)
        {
            perror("read");
            break;
        }

        std::string old_file_name;
        for(int i = 0; i < length; i += eventSize + ((struct inotify_event *)&buffer[i])->len)
        {
            auto *event = (struct inotify_event *)&buffer[i];
            if(event->mask&(IN_CREATE))
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] file {} created, updating file map...", event->name);
                addFileToStartTimeFileNameMap(event->name);
            }
            else if(event->mask&(IN_DELETE))
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] file {} deleted, updating file map...", event->name);
                removeFileFromStartTimeFileNameMap(event->name);
            }
            else if(event->mask&(IN_MOVED_FROM))
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] file is renamed from {}", event->name);
                old_file_name = event->name;
            }
            else if(event->mask&(IN_MOVED_TO))
            {
                if(old_file_name.empty())
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] file {} created, updating file map...", event->name);
                    addFileToStartTimeFileNameMap(event->name);
                }
                else
                {
                    LOG_DEBUG("[HDF5ArchiveReadingAgent] file is renamed to {}, updating file map...",event->name);
                    std::string new_file_name = event->name;
                    renameFileInStartTimeFileNameMap(old_file_name, new_file_name);
                    old_file_name.clear();
                }
            }
        }
    }

    inotify_rm_watch(inotifyFd, watchFd);
    close(inotifyFd);

    return 0;
}
} // chronolog
