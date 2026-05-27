#include <filesystem>
#include <json-c/json.h>
#include <regex>
#include <string>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>
#include <HDF5FileChunkExtractor.h>

namespace tl = thallium;

namespace chl = chronolog;

chronolog::HDF5FileChunkExtractor::HDF5FileChunkExtractor(const std::string& hdf5_files_root_dir)
    : rootDirectory(hdf5_files_root_dir)
{
    LOG_TRACE("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}

chronolog::HDF5FileChunkExtractor::~HDF5FileChunkExtractor()
{
    LOG_TRACE("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}
//////

int chronolog::HDF5FileChunkExtractor::reset(std::string const& new_archive_dir)
{
    rootDirectory = new_archive_dir;
    LOG_INFO("HDF5FileChunkExtractor] Reset success: using directory :", rootDirectory);
    return chl::CL_SUCCESS;
}

// json block for HDF5 File Chunk Extractor looks like this
//
//  "extractor_name": {
//                "type": "hdf5_extractor",
//                "hdf5_archive_dir": "/tmp/hdf5_archive"
//               }
//
//////////

int chronolog::HDF5FileChunkExtractor::reset(json_object* json_block)
{
    if((json_block == nullptr) || !json_object_is_type(json_block, json_type_object) ||
       (json_object_object_get(json_block, "type") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "type"), json_type_string) ||
       (std::string("hdf5_extractor").compare(json_object_get_string(json_object_object_get(json_block, "type"))) != 0))
    {
        rootDirectory = "/tmp";
        LOG_ERROR("HDF5FileChunkExtractor] Reset failure: invalid json_conf; using {}", rootDirectory);
        return chl::CL_ERR_INVALID_CONF;
    }

    if((json_object_object_get(json_block, "hdf5_archive_dir") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "hdf5_archive_dir"), json_type_string))
    {
        rootDirectory = "/tmp";
        LOG_ERROR("HDF5FileChunkExtractor] Reset failure: invalid json_conf; using {} ", rootDirectory);
        return chl::CL_ERR_INVALID_CONF;
    }

    rootDirectory = json_object_get_string(json_object_object_get(json_block, "hdf5_archive_dir"));

    // check if archive directory exists and is writable by the extractor process
    if(!std::filesystem::exists(rootDirectory))
    {
        rootDirectory = "/tmp";
        LOG_ERROR("HDF5FileChunkExtractor] Reset failure: hdf5_archive_dir doesn't exist or not writable; using {} ",
                  rootDirectory);
        return chl::CL_ERR_INVALID_CONF;
    }

    LOG_INFO("HDF5FileChunkExtractor] Reset success: using {}", rootDirectory);
    return chl::CL_SUCCESS;
}

//////


namespace
{
// Escape characters that have special meaning in std::regex so chronicle/story
// names containing them (e.g. dots) are matched literally rather than as
// metacharacters.
std::string regex_escape(std::string const& in)
{
    return std::regex_replace(in, std::regex(R"([.^$|()\\*+?{}\[\]])"), R"(\$&)");
}

int delete_matching_files(std::string const& root_directory,
                          std::regex const& filename_pattern,
                          std::string const& what,
                          size_t* deleted_count)
{
    size_t local_count = 0;
    if(!std::filesystem::exists(root_directory))
    {
        LOG_DEBUG("[HDF5FileChunkExtractor] Archive directory {} does not exist; nothing to delete for {}",
                  root_directory,
                  what);
        if(deleted_count != nullptr)
        {
            *deleted_count = 0;
        }
        return chl::CL_SUCCESS;
    }

    // std::filesystem::directory_iterator's constructor reports an open
    // failure (e.g. EACCES on the archive directory) by setting `ec` and
    // returning an end-iterator. Previously we only checked `ec` inside the
    // loop body, so a failed open looked like an empty directory and
    // delete_*_files returned CL_SUCCESS with zero deletions -- the metadata
    // would be gone but the files would still be on disk. Check `ec`
    // immediately after construction.
    std::error_code ec;
    std::filesystem::directory_iterator it(root_directory, ec);
    if(ec)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Failed to open directory {}: {}", root_directory, ec.message());
        return chl::CL_ERR_UNKNOWN;
    }
    for(auto const& entry: it)
    {
        if(!entry.is_regular_file())
        {
            continue;
        }
        std::string const filename = entry.path().filename().string();
        if(!std::regex_match(filename, filename_pattern))
        {
            continue;
        }
        std::error_code rm_ec;
        std::filesystem::remove(entry.path(), rm_ec);
        if(rm_ec)
        {
            LOG_ERROR("[HDF5FileChunkExtractor] Failed to delete {}: {}", entry.path().string(), rm_ec.message());
            return chl::CL_ERR_UNKNOWN;
        }
        LOG_INFO("[HDF5FileChunkExtractor] Deleted {} ({})", entry.path().string(), what);
        ++local_count;
    }
    if(deleted_count != nullptr)
    {
        *deleted_count = local_count;
    }
    return chl::CL_SUCCESS;
}
} // namespace

int chronolog::HDF5FileChunkExtractor::delete_story_files(std::string const& chronicle_name,
                                                          std::string const& story_name,
                                                          size_t* deleted_count)
{
    // Matches the filename layout produced by StoryChunkWriter::writeStoryChunk:
    //   <chronicle>.<story>.<startSec>.vlen.h5 (and rotated <...>.<n>.vlen.h5).
    std::string const pattern_str =
            regex_escape(chronicle_name) + "\\." + regex_escape(story_name) + "\\.[0-9]+(\\.[0-9]+)?\\.vlen\\.h5";
    std::regex const filename_pattern(pattern_str);
    std::string const what = "story " + chronicle_name + "/" + story_name;
    return delete_matching_files(rootDirectory, filename_pattern, what, deleted_count);
}

int chronolog::HDF5FileChunkExtractor::delete_chronicle_files(std::string const& chronicle_name, size_t* deleted_count)
{
    // Matches any story under the chronicle:
    //   <chronicle>.<anyStory>.<startSec>(.<n>)?.vlen.h5
    std::string const pattern_str = regex_escape(chronicle_name) + "\\.[^.]+\\.[0-9]+(\\.[0-9]+)?\\.vlen\\.h5";
    std::regex const filename_pattern(pattern_str);
    std::string const what = "chronicle " + chronicle_name;
    return delete_matching_files(rootDirectory, filename_pattern, what, deleted_count);
}

int chronolog::HDF5FileChunkExtractor::process_chunk(chl::StoryChunk* story_chunk)
{
    LOG_INFO("[HDF5FileChunkExtractor] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",
             thallium::thread::self_id(),
             story_chunk->getStoryId(),
             story_chunk->getChronicleName(),
             story_chunk->getStoryName(),
             story_chunk->getStartTime(),
             story_chunk->getEndTime(),
             story_chunk->getEventCount());

    StoryChunkWriter chunkWriter(rootDirectory, "story_chunks", "data");
    hsize_t size = chunkWriter.writeStoryChunk(*story_chunk);
    if(size == 0)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Error writing StoryChunk to file: StoryId={} {}-{} {}-{} eventCount {}",
                  story_chunk->getStoryId(),
                  story_chunk->getChronicleName(),
                  story_chunk->getStoryName(),
                  story_chunk->getStartTime(),
                  story_chunk->getEndTime(),
                  story_chunk->getEventCount());
        return chl::CL_ERR_UNKNOWN;
    }
    else
    {
        LOG_INFO("[HDF5FileChunkExtractor] StoryChunk written to file: StoryId={} {}-{} {}-{} eventCount {}",
                 story_chunk->getStoryId(),
                 story_chunk->getChronicleName(),
                 story_chunk->getStoryName(),
                 story_chunk->getStartTime(),
                 story_chunk->getEndTime(),
                 story_chunk->getEventCount());
        return chl::CL_SUCCESS;
    }
}
