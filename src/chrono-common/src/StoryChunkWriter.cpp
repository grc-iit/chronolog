#include <algorithm>
#include <atomic>
#include <filesystem>
#include <regex>
#include <stdexcept>
#include <unistd.h>

#include <HDF5FileAccess.h>
#include <StoryChunkWriter.h>

namespace fs = std::filesystem;

namespace chronolog
{
namespace
{
// A window is written under this name and renamed into place only once it is
// complete, so a player listing the archive directory never opens a partial
// file, and a write that fails leaves nothing for a later replay to trip over.
// The suffix keeps it out of the player's listing and out of the numbered-file
// scan in getStoryChunkFileName, and pid plus a counter keep two writers from
// sharing one temporary.
std::string partialFileName(std::string const& final_name)
{
    static std::atomic<uint64_t> sequence{0};
    return final_name + ".partial." + std::to_string(::getpid()) + "." + std::to_string(sequence++);
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
} // namespace
hsize_t StoryChunkWriter::writeStoryChunk(StoryChunkHVL& story_chunk)
{
    std::vector<LogEventHVL> data;
    data.reserve(story_chunk.getEventCount());
    for(const auto& start: story_chunk) { data.push_back(start.second); }
    std::string file_name = rootDirectory + story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                            std::to_string(story_chunk.getStartTime() / 1000000000) + ".vlen.h5";
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
        std::error_code ec;
        fs::rename(partial_name, file_name, ec);
        if(ec)
        {
            LOG_ERROR("[StoryChunkWriter] Could not move {} into place as {}: {}",
                      partial_name,
                      file_name,
                      ec.message());
            removePartialFile(partial_name);
            return 0;
        }

        LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
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

std::string StoryChunkWriter::getStoryChunkFileName(std::string const& root_dir, std::string const& base_file_name)
{
    const std::string base_file_name_no_ext = fs::path(base_file_name).stem().make_preferred().string();
    const std::string escaped_base_file_name_no_ext =
            std::regex_replace(base_file_name_no_ext, std::regex(R"([.^$|()\\*+?{}\[\]])"), R"(\$&)");
    const std::string rotated_prefix = escaped_base_file_name_no_ext + "\\.";
    const std::string ext = fs::path(base_file_name).extension().string();

    const std::regex rotated_pattern("^" + rotated_prefix + "([0-9]+)" + ext + "$");

    long long max_n = 0; // Means no numbered files have been found yet.
    bool base_exists = false;

    std::error_code ec;
    for(const auto& entry: fs::directory_iterator(root_dir, ec))
    {
        if(!entry.is_regular_file(ec))
        {
            continue; // Skip directories, symlinks, etc.
        }

        const std::string filename = entry.path().filename().string();

        // Check for the base file
        if(filename == base_file_name)
        {
            base_exists = true;
            continue;
        }

        // Check for rotated files
        std::smatch match;
        if(std::regex_match(filename, match, rotated_pattern))
        {
            // match[0] is the entire string, match[1] is the first capture group.
            if(match.size() == 2)
            {
                try
                {
                    long long current_n = std::stoll(match[1].str());
                    max_n = std::max(max_n, current_n);
                }
                catch(const std::out_of_range&)
                {
                    LOG_ERROR("[StoryChunkWriter] Number in file name '{}' is out of range.", match[1].str());
                }
            }
        }
    }

    fs::path next_filename_no_ext;
    if(!base_exists)
    {
        next_filename_no_ext = base_file_name_no_ext;
    }
    else
    {
        // We found numbered files, so the next is max_n + 1.
        next_filename_no_ext = base_file_name_no_ext + "." + std::to_string(max_n + 1);
    }

    next_filename_no_ext += ext;
    LOG_DEBUG("[StoryChunkWriter] Next unique file name: {}", next_filename_no_ext.string());
    return (root_dir / next_filename_no_ext).make_preferred();
}

hsize_t StoryChunkWriter::writeStoryChunk(StoryChunk& story_chunk)
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
    std::string file_name = story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                            std::to_string(story_chunk.getStartTime() / 1000000000) + ".vlen.h5";
    //    file_name = fs::path(rootDirectory) / fs::path(file_name);
    std::string partial_name;
    std::unique_ptr<H5::H5File> file;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Making sure the StoryChunk file name is unique...");
        file_name = getStoryChunkFileName(rootDirectory, file_name);
        partial_name = partialFileName(file_name);

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
        std::error_code ec;
        fs::rename(partial_name, file_name, ec);
        if(ec)
        {
            LOG_ERROR("[StoryChunkWriter] Could not move {} into place as {}: {}",
                      partial_name,
                      file_name,
                      ec.message());
            removePartialFile(partial_name);
            return 0;
        }

        LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
        return file_size;
    }
    catch(H5::Exception const& error)
    {
        // any step can fail on a full or failing disk, and flush throws a
        // LocationException, not a FileIException
        LOG_ERROR("[StoryChunkWriter] {} failed for {}: {}", error.getCFuncName(), file_name, error.getCDetailMsg());
        H5::Exception::printErrorStack();
    }
    if(!partial_name.empty())
    {
        removePartialFile(partial_name);
    }
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