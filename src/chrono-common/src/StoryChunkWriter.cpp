#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <unistd.h>

#include <ArchiveLayout.h>
#include <HDF5FileAccess.h>
#include <StoryChunkWriter.h>

namespace fs = std::filesystem;

namespace chronolog
{
namespace
{
std::string hostName()
{
    char name[256] = {};
    if(::gethostname(name, sizeof(name) - 1) != 0 || name[0] == '\0')
    {
        return "unknown-host";
    }
    return name;
}

// A window is written under this name and moved into place only once it is
// complete, so a player listing the archive directory never opens a partial
// file, and a write that fails leaves nothing for a later replay to trip over.
// The suffix keeps it out of the player's listing and out of the window names
// publishFile tries. Host, pid and a counter keep two writers from sharing one
// temporary: graphers on different nodes write into the same shared directory,
// and each in its own container can have the same pid.
std::string partialFileName(std::string const& base_path)
{
    static std::atomic<uint64_t> sequence{0};
    static std::string const host = hostName();
    return base_path + ".partial." + host + "." + std::to_string(::getpid()) + "." + std::to_string(sequence++);
}

void removePartialFile(std::string const& partial_name)
{
    std::error_code ec;
    fs::remove(partial_name, ec);
    if(ec)
    {
        LOG_ERROR("[StoryChunkWriter] Could not remove the partial file {}: {}", partial_name, ec.message());
    }
}

// The n-th name tried for a file: its own name first, then <stem>.<n>.h5. File
// names are never reused, so only a link that NFS reported failed after it
// made it (see publishFile) needs another.
std::string numberedFileName(fs::path const& root_dir, std::string const& base_file_name, uint64_t n)
{
    if(n == 0)
    {
        return (root_dir / base_file_name).string();
    }
    fs::path const base(base_file_name);
    return (root_dir / (base.stem().string() + "." + std::to_string(n) + base.extension().string())).string();
}

// Far more files than one window ever gets; only a directory in a bad state
// reaches it.
constexpr uint64_t kMaxFilesPerWindow = 100000;

// For a file system without hard links: the first name that does not exist,
// taken by rename. Safe between the threads of this process only.
std::mutex renameFallbackMutex;

bool publishByRename(std::string const& partial_name,
                     fs::path const& root_dir,
                     std::string const& base_file_name,
                     std::string& file_name)
{
    std::lock_guard<std::mutex> lock(renameFallbackMutex);
    for(uint64_t n = 0; n < kMaxFilesPerWindow; ++n)
    {
        file_name = numberedFileName(root_dir, base_file_name, n);
        std::error_code ec;
        bool const taken = fs::exists(file_name, ec);
        if(!ec && taken)
        {
            continue;
        }
        if(!ec)
        {
            fs::rename(partial_name, file_name, ec);
        }
        if(ec)
        {
            LOG_ERROR("[StoryChunkWriter] Could not move {} into place as {}: {}",
                      partial_name,
                      file_name,
                      ec.message());
            return false;
        }
        return true;
    }
    LOG_ERROR("[StoryChunkWriter] No free name left for {}", partial_name);
    return false;
}

// Moves a complete window file into place under its name, or the first free
// numbered one. Names are not shared between writes (see ArchiveLayout.h), so
// the name is normally free; it can be taken when NFS answers a link whose
// reply was lost and that was sent again with EEXIST, or by a process forked
// from this one, which inherits its writer tag and sequence. A hard link fails
// with EEXIST instead of replacing a file, atomically on the server for NFS, so
// no write can take the place of another's; the partial name is removed once
// the link stands. Sets file_name to the name the window got.
bool publishFile(std::string const& partial_name,
                 fs::path const& root_dir,
                 std::string const& base_file_name,
                 std::string& file_name)
{
    for(uint64_t n = 0; n < kMaxFilesPerWindow; ++n)
    {
        file_name = numberedFileName(root_dir, base_file_name, n);
        std::error_code ec;
        fs::create_hard_link(partial_name, file_name, ec);
        if(!ec)
        {
            removePartialFile(partial_name);
            return true;
        }
        if(ec == std::errc::file_exists)
        {
            // Over NFS a link whose reply was lost is sent again and answered
            // EEXIST, although the first one made it; the partial file then has
            // this second name.
            std::error_code count_ec;
            std::uintmax_t const links = fs::hard_link_count(partial_name, count_ec);
            if(!count_ec && links > 1)
            {
                removePartialFile(partial_name);
                return true;
            }
            continue;
        }
        if(ec == std::errc::operation_not_permitted || ec == std::errc::operation_not_supported ||
           ec == std::errc::not_supported)
        {
            static std::once_flag warned;
            std::call_once(warned,
                           [&]()
                           {
                               LOG_WARNING("[StoryChunkWriter] The archive file system does not allow hard links "
                                           "({}); window files are moved into place by rename, which keeps two "
                                           "writers of one window apart only within this process",
                                           ec.message());
                           });
            return publishByRename(partial_name, root_dir, base_file_name, file_name);
        }
        LOG_ERROR("[StoryChunkWriter] Could not move {} into place as {}: {}", partial_name, file_name, ec.message());
        return false;
    }
    LOG_ERROR("[StoryChunkWriter] No free name left for {}", partial_name);
    return false;
}
// Creates the chronicle and story directories under the archive root, but not
// the root itself: a missing root is a misconfiguration, or an archive file
// system that is not mounted, and writing under its mount point would fill the
// local disk instead.
bool createStoryDirectory(fs::path const& root_dir, fs::path const& story_dir)
{
    std::error_code ec;
    if(!fs::is_directory(root_dir, ec))
    {
        LOG_ERROR("[StoryChunkWriter] The archive directory {} does not exist", root_dir.string());
        return false;
    }
    fs::create_directories(story_dir, ec);
    if(ec)
    {
        LOG_ERROR("[StoryChunkWriter] Could not create {}: {}", story_dir.string(), ec.message());
        return false;
    }
    return true;
}
} // namespace
std::string StoryChunkWriter::writerTag(std::string const& recording_group)
{
    static uint64_t const process_start = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
    return recording_group + "." + std::to_string(process_start);
}

hsize_t StoryChunkWriter::writeStoryChunk(StoryChunkHVL& story_chunk)
{
    std::vector<LogEventHVL> data;
    data.reserve(story_chunk.getEventCount());
    for(const auto& start: story_chunk) { data.push_back(start.second); }
    return writeWindow(story_chunk.getChronicleName(),
                       story_chunk.getStoryName(),
                       story_chunk.getStartTime(),
                       0,
                       data,
                       nullptr);
}

hsize_t StoryChunkWriter::writeStoryChunk(StoryChunk& story_chunk, std::string* published_file)
{
    std::vector<LogEventHVL> data;
    data.reserve(story_chunk.getEventCount());
    for(const auto& event: story_chunk)
    {
        hvl_t log_record;
        log_record.len = event.second.logRecord.size();
        log_record.p = (void*)event.second.logRecord.data();
        data.emplace_back(event.second.getStoryId(),
                          event.second.time(),
                          event.second.getClientId(),
                          event.second.index(),
                          log_record);
    }
    return writeWindow(story_chunk.getChronicleName(),
                       story_chunk.getStoryName(),
                       story_chunk.getStartTime(),
                       story_chunk.getIncarnation(),
                       data,
                       published_file);
}

hsize_t StoryChunkWriter::writeWindow(std::string const& chronicle_name,
                                      std::string const& story_name,
                                      uint64_t start_time,
                                      uint64_t incarnation,
                                      std::vector<LogEventHVL>& data,
                                      std::string* published_file)
{
    // every file this process publishes gets its own number
    static std::atomic<uint64_t> sequence{0};
    // the story's directory, created on its first window
    fs::path const story_dir = storyArchiveDirectory(rootDirectory, chronicle_name, story_name);
    std::string const base_file_name = windowFileName(start_time, writerTagValue, incarnation, sequence++);
    std::string file_name = (story_dir / base_file_name).string();
    if(!createStoryDirectory(rootDirectory, story_dir))
    {
        return 0;
    }
    std::string const partial_name = partialFileName(file_name);
    std::unique_ptr<H5::H5File> file;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating StoryChunk file: {} (as {})", file_name, partial_name);
        file = std::make_unique<H5::H5File>(partial_name,
                                            H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE,
                                            H5::FileCreatPropList::DEFAULT,
                                            archiveFileAccess());

        LOG_DEBUG("[StoryChunkWriter] Writing StoryChunk to file...");
        if(writeEvents(file, data) == 0)
        {
            LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to file.");
            file->close();
            removePartialFile(partial_name);
            return 0;
        }

        file->flush(H5F_SCOPE_GLOBAL);
        hsize_t const file_size = file->getFileSize();
        // closed here so that a failed close counts as a failed write
        file->close();

        // the window appears under its own name complete or not at all
        if(!publishFile(partial_name, story_dir, base_file_name, file_name))
        {
            removePartialFile(partial_name);
            return 0;
        }

        LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
        if(published_file != nullptr)
        {
            *published_file = file_name;
        }
        return file_size;
    }
    catch(H5::Exception const& error)
    {
        // any step can fail on a full or failing disk, and flush throws a
        // LocationException, not a FileIException
        LOG_ERROR("[StoryChunkWriter] {} failed for {}: {}", error.getCFuncName(), file_name, error.getCDetailMsg());
        H5::Exception::printErrorStack();
    }
    removePartialFile(partial_name);
    return 0;
}

hsize_t StoryChunkWriter::writeEvents(std::unique_ptr<H5::H5File>& file, std::vector<LogEventHVL>& data)
{
    int ret = 0;
    try
    {
        /*
         * Create a group in the file
        */
        LOG_DEBUG("[StoryChunkWriter] Creating group: {}", groupName);
        auto* group = new H5::Group(file->createGroup(groupName));

        hsize_t dim_size = data.size();
        LOG_DEBUG("[StoryChunkWriter] Creating dataspace with size: {}", dim_size);
        auto* dataspace = new H5::DataSpace(numDims, &dim_size);

        // target dtype for the file
        LOG_DEBUG("[StoryChunkWriter] Creating data type for events...");
        H5::CompType data_type = createEventCompoundType();

        LOG_DEBUG("[StoryChunkWriter] Creating dataset: {}", dsetName);
        auto* dataset = new H5::DataSet(
                file->createDataSet("/" + groupName + "/" + dsetName + ".vlen_bytes", data_type, *dataspace));

        LOG_DEBUG("[StoryChunkWriter] Writing data to dataset...");
        dataset->write(&data.front(), data_type);

        delete dataset;
        delete dataspace;
        delete group;

        return data.size();
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    catch(H5::DataSetIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSetIException: {}", error.getCDetailMsg());
        H5::DataSetIException::printErrorStack();
    }
    catch(H5::DataSpaceIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSpaceIException: {}", error.getCDetailMsg());
        H5::DataSpaceIException::printErrorStack();
    }
    catch(H5::DataTypeIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataTypeIException: {}", error.getCDetailMsg());
        H5::DataTypeIException::printErrorStack();
    }
    return ret;
}

} // namespace chronolog