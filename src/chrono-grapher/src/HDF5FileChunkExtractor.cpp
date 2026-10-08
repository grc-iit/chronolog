#include <filesystem>
#include <json-c/json.h>
#include <string>
#include <vector>
#include <thallium.hpp>

#include <ArchiveLayout.h>
#include <ArchiveManifest.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>
#include <HDF5FileChunkExtractor.h>
#include <StoryWatermarkRegistry.h>

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

int chronolog::HDF5FileChunkExtractor::openArchiveManifest(std::string const& writer_id)
{
    auto manifest = std::make_shared<ArchiveManifestWriter>(rootDirectory, writer_id);
    int const status = manifest->open();
    if(status != chl::CL_SUCCESS)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Could not open the archive manifest log {}", manifest->logPath());
        return status;
    }
    LOG_INFO("[HDF5FileChunkExtractor] Recording published files in {}", manifest->logPath());
    archiveManifest = std::move(manifest);
    fileWriterTag = StoryChunkWriter::writerTag(writer_id);
    return chl::CL_SUCCESS;
}

int chronolog::HDF5FileChunkExtractor::recordDeletion(std::string const& chronicle_name, std::string const* story_name)
{
    if(archiveManifest == nullptr)
    {
        return chl::CL_SUCCESS;
    }
    ArchiveManifestRecord record;
    record.op = ArchiveManifestRecord::Op::DELETE;
    record.chronicle = chronicle_name;
    record.whole_chronicle = (story_name == nullptr);
    if(story_name != nullptr)
    {
        record.story = *story_name;
    }
    return archiveManifest->append(record);
}

//////


// Deletes the archive's files in one story directory (isWindowFileName),
// adding the window files to count. Nothing else is touched: the
// directory is named after a client's names, and the archive root may be a
// directory other programs use (the template's is /tmp). The directory itself
// stays, so that no path a player has looked up comes back after it was
// removed (see ArchiveLayout.h). Every grapher sharing the archive runs the
// same destroy, so a file or directory already gone is not an error.
int chronolog::HDF5FileChunkExtractor::deleteStoryDirectory(std::filesystem::path const& dir,
                                                            std::string const& what,
                                                            size_t& count)
{
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for(std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code type_ec;
        if(it->is_regular_file(type_ec) && chl::isWindowFileName(it->path().filename().string()))
        {
            files.push_back(it->path());
        }
    }
    if(ec == std::errc::no_such_file_or_directory)
    {
        return chl::CL_SUCCESS;
    }
    if(ec)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Cannot list {} ({}): {}", dir.string(), what, ec.message());
        return chl::CL_ERR_UNKNOWN;
    }
    for(auto const& file: files)
    {
        std::error_code rm_ec;
        bool const removed = std::filesystem::remove(file, rm_ec);
        if(rm_ec && rm_ec != std::errc::no_such_file_or_directory)
        {
            LOG_ERROR("[HDF5FileChunkExtractor] Failed to delete {} ({}): {}", file.string(), what, rm_ec.message());
            return chl::CL_ERR_UNKNOWN;
        }
        if(removed && !chl::isPartialFileName(file.filename().string()))
        {
            ++count;
        }
    }
    LOG_INFO("[HDF5FileChunkExtractor] Deleted the archive files in {} ({})", dir.string(), what);
    return chl::CL_SUCCESS;
}

int chronolog::HDF5FileChunkExtractor::delete_story_files(std::string const& chronicle_name,
                                                          std::string const& story_name,
                                                          size_t* deleted_count)
{
    std::string const what = "story " + chronicle_name + "/" + story_name;
    size_t count = 0;
    int const status =
            deleteStoryDirectory(storyArchiveDirectory(rootDirectory, chronicle_name, story_name), what, count);
    if(deleted_count != nullptr)
    {
        *deleted_count = count;
    }
    return (status == chl::CL_SUCCESS) ? recordDeletion(chronicle_name, &story_name) : status;
}

int chronolog::HDF5FileChunkExtractor::delete_chronicle_files(std::string const& chronicle_name, size_t* deleted_count)
{
    std::string const what = "chronicle " + chronicle_name;
    std::filesystem::path const dir = chronicleArchiveDirectory(rootDirectory, chronicle_name);
    size_t count = 0;
    std::error_code ec;
    std::vector<std::filesystem::path> story_dirs;
    for(std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code type_ec;
        if(it->is_directory(type_ec))
        {
            story_dirs.push_back(it->path());
        }
    }
    int status = chl::CL_SUCCESS;
    if(ec && ec != std::errc::no_such_file_or_directory)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Cannot list {} ({}): {}", dir.string(), what, ec.message());
        status = chl::CL_ERR_UNKNOWN;
    }
    for(auto const& story_dir: story_dirs)
    {
        if(status != chl::CL_SUCCESS)
        {
            break;
        }
        status = deleteStoryDirectory(story_dir, what, count);
    }
    if(deleted_count != nullptr)
    {
        *deleted_count = count;
    }
    return (status == chl::CL_SUCCESS) ? recordDeletion(chronicle_name, nullptr) : status;
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

    if(story_chunk->empty())
    {
        // an idle-gap window: nothing to write, but the interval is vacuously
        // durable and must extend the persisted watermark or the contiguous
        // prefix would hold at the gap forever
        if(watermarkRegistry != nullptr && !story_chunk->isWatermarkExempt())
        {
            watermarkRegistry->advancePersisted(story_chunk->getStoryId(),
                                                story_chunk->getStartTime(),
                                                story_chunk->getEndTime());
        }
        // An empty window can still hold receipts: their events sorted into the
        // neighbouring window, which holds them too. Let go here, or the
        // receipt waits on a holder that will never be written.
        if(watermarkRegistry != nullptr)
        {
            for(uint64_t receipt: story_chunk->carriedReceipts())
            {
                watermarkRegistry->releaseReceipt(story_chunk->getStoryId(), receipt);
            }
        }
        return chl::CL_SUCCESS;
    }

    StoryChunkWriter chunkWriter(rootDirectory,
                                 "story_chunks",
                                 "data",
                                 fileWriterTag.empty() ? StoryChunkWriter::writerTag("0") : fileWriterTag);
    std::string published_file;
    hsize_t size = chunkWriter.writeStoryChunk(*story_chunk, &published_file);
    if(size != 0 && archiveManifest != nullptr)
    {
        // Before W moves and receipts settle: once they do, keepers free the
        // chunk, and a player finds the file only through this record. A file
        // no record names is removed and the window counts as failed.
        ArchiveManifestRecord record;
        record.op = ArchiveManifestRecord::Op::PUBLISH;
        record.chronicle = story_chunk->getChronicleName();
        record.story = story_chunk->getStoryName();
        record.file = std::filesystem::path(published_file).lexically_relative(rootDirectory).string();
        record.start = story_chunk->getStartTime();
        record.end = story_chunk->getEndTime();
        record.events = story_chunk->getEventCount();
        if(archiveManifest->append(record) != chl::CL_SUCCESS)
        {
            LOG_ERROR("[HDF5FileChunkExtractor] Could not record {} in the archive manifest; removing it",
                      published_file);
            std::error_code ec;
            std::filesystem::remove(published_file, ec);
            size = 0;
        }
    }
    if(size == 0)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Error writing StoryChunk to file: StoryId={} {}-{} {}-{} eventCount {}",
                  story_chunk->getStoryId(),
                  story_chunk->getChronicleName(),
                  story_chunk->getStoryName(),
                  story_chunk->getStartTime(),
                  story_chunk->getEndTime(),
                  story_chunk->getEventCount());
        if(watermarkRegistry != nullptr && !story_chunk->isWatermarkExempt())
        {
            watermarkRegistry->persistFailed(story_chunk->getStoryId());
        }
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
        // StoryChunkWriter flushes H5F_SCOPE_GLOBAL before returning, so the
        // window counts as persisted here. Salvage chunks are exempt: they are
        // one keeper's rescued events, not a merged timeline window.
        if(watermarkRegistry != nullptr && !story_chunk->isWatermarkExempt())
        {
            watermarkRegistry->advancePersisted(story_chunk->getStoryId(),
                                                story_chunk->getStartTime(),
                                                story_chunk->getEndTime());
        }
        // one write fewer for every keeper chunk whose events this chunk
        // holds, salvage chunks included: W cannot confirm those
        if(watermarkRegistry != nullptr)
        {
            for(uint64_t receipt: story_chunk->carriedReceipts())
            {
                watermarkRegistry->releaseReceipt(story_chunk->getStoryId(), receipt);
            }
        }
        return chl::CL_SUCCESS;
    }
}
