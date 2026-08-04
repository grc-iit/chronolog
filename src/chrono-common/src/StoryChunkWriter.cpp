#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <regex>
#include <stdexcept>

#include <unistd.h>

#include <StoryChunkWriter.h>

namespace fs = std::filesystem;

namespace chronolog
{
hsize_t StoryChunkWriter::writeStoryChunk(StoryChunkHVL& story_chunk)
{
    std::vector<LogEventHVL> data;
    data.reserve(story_chunk.getEventCount());
    for(const auto& start: story_chunk) { data.push_back(start.second); }
    std::string file_name = rootDirectory + story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                            std::to_string(story_chunk.getStartTime() / 1000000000) + ".vlen.h5";
    hsize_t ret = 0;
    std::unique_ptr<H5::H5File> file;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating StoryChunk file: {}", file_name);
        file = std::make_unique<H5::H5File>(file_name, H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE);

        LOG_DEBUG("[StoryChunkWriter] Writing StoryChunk to file...");
        ret = writeEvents(file, data);
        if(ret == 0)
        {
            LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to file.");
            return ret;
        }

        file->flush(H5F_SCOPE_GLOBAL);
        hsize_t file_size = file->getFileSize();

        LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
        return file_size;
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    return ret;
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

// link() rather than rename(): rename REPLACES an existing destination, and
// extraction runs on several streams, so between choosing a free name and
// publishing it another stream can take that name -- a replace would then
// silently destroy an already-archived chunk. link() fails with EEXIST instead,
// leaving the caller to pick the next rotation index.
bool StoryChunkWriter::publishFile(std::string const& temp_path, std::string const& final_path)
{
    return ::link(temp_path.c_str(), final_path.c_str()) == 0;
}

std::string StoryChunkWriter::temporaryFileNameFor(std::string const& final_file_name)
{
    // pid keeps two processes sharing an archive root apart; the counter keeps the
    // extraction module's own streams apart. Suffix order matters: ".h5.<...>.tmp"
    // ends in ".tmp", so neither the Player's extension check nor the rotation
    // regex (which anchors on ".h5") can see it.
    static std::atomic<uint64_t> counter{0};
    return final_file_name + "." + std::to_string(::getpid()) + "." +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
}

StoryChunkWriteResult StoryChunkWriter::writeStoryChunk(StoryChunk& story_chunk)
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
    std::string const base_file_name = story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                                       std::to_string(story_chunk.getStartTime() / 1000000000) + ".vlen.h5";

    StoryChunkWriteResult result;
    if(data.empty())
    {
        // Nothing to publish. Returning early also keeps an empty chunk from
        // leaving a stray temporary behind.
        LOG_DEBUG("[StoryChunkWriter] StoryChunk has no events; no file written.");
        return result;
    }

    std::string temp_path;
    try
    {
        // Build under a name the Player cannot mistake for archive content. The
        // final name is not chosen yet on purpose: choosing it here and creating
        // the file to claim it is what would expose a partial file.
        temp_path = temporaryFileNameFor((fs::path(rootDirectory) / fs::path(base_file_name)).string());

        LOG_DEBUG("[StoryChunkWriter] Building StoryChunk file at {}", temp_path);
        std::unique_ptr<H5::H5File> file = std::make_unique<H5::H5File>(temp_path, H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE);

        hsize_t const written = writeEvents(file, data);
        if(written == 0)
        {
            LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to {}", temp_path);
            file.reset();
            std::error_code ec;
            fs::remove(temp_path, ec);
            return result;
        }

        file->flush(H5F_SCOPE_GLOBAL);
        result.file_size = file->getFileSize();
        file.reset(); // close before publishing
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
        if(!temp_path.empty())
        {
            std::error_code ec;
            fs::remove(temp_path, ec);
        }
        return StoryChunkWriteResult{};
    }

    // Publish. link() is used rather than rename() because rename REPLACES an
    // existing destination: extraction runs on several streams, so between
    // picking a free name and publishing, another stream can publish the same
    // one, and a replace would silently destroy an already-archived chunk.
    // link() fails with EEXIST instead, and the loop simply takes the next
    // rotation index.
    std::string published;
    for(int attempt = 0; attempt < PUBLISH_ATTEMPTS; ++attempt)
    {
        std::string const candidate = getStoryChunkFileName(rootDirectory, base_file_name);
        if(publishFile(temp_path, candidate))
        {
            published = candidate;
            break;
        }
        if(errno != EEXIST)
        {
            LOG_ERROR("[StoryChunkWriter] Failed to publish {} as {}: {}", temp_path, candidate, strerror(errno));
            break;
        }
        LOG_DEBUG("[StoryChunkWriter] {} was taken between choosing and publishing; rotating", candidate);
    }

    std::error_code ec;
    fs::remove(temp_path, ec); // the link (or the failure) is what owns the data now

    if(published.empty())
    {
        LOG_ERROR("[StoryChunkWriter] Gave up publishing a StoryChunk for {}", base_file_name);
        return StoryChunkWriteResult{};
    }

    result.file_name = published;
    result.seq = rotationIndexOf(fs::path(published).filename().string());
    LOG_DEBUG("[StoryChunkWriter] Published StoryChunk as {} ({} bytes)", published, result.file_size);
    return result;
}

// "<chronicle>.<story>.<startSec>.vlen.h5" is index 0; a rotation carries the
// index just before the extension: "<...>.vlen.<n>.h5".
uint32_t StoryChunkWriter::rotationIndexOf(std::string const& file_name)
{
    static std::regex const rotated(R"(\.vlen\.([0-9]+)\.h5$)");
    std::smatch match;
    if(std::regex_search(file_name, match, rotated) && match.size() == 2)
    {
        try
        {
            return static_cast<uint32_t>(std::stoul(match[1].str()));
        }
        catch(std::exception const&)
        {
            return 0;
        }
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