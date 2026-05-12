#include <string>
#include <thallium.hpp>

#include <StoryChunk.h>
#include <StoryChunkWriter.h>
#include <HDF5FileChunkExtractor.h>
#include <chronolog_profile.h>

namespace tl = thallium;

namespace chl = chronolog;

chronolog::HDF5FileChunkExtractor::HDF5FileChunkExtractor(const std::string& hdf5_files_root_dir)
    : rootDirectory(hdf5_files_root_dir)
{
    LOG_DEBUG("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}

chronolog::HDF5FileChunkExtractor::~HDF5FileChunkExtractor()
{
    LOG_DEBUG("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}

int chronolog::HDF5FileChunkExtractor::process_chunk(chl::StoryChunk* story_chunk)
{
    CL_PROFILE_REGION("grapher_archive_chunk");
    CL_PROFILE_COUNTER("grapher_archive_chunk_events", story_chunk->getEventCount());

    LOG_INFO("[HDF5FileChunkExtractor] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",
             thallium::thread::self_id(),
             story_chunk->getStoryId(),
             story_chunk->getChronicleName(),
             story_chunk->getStoryName(),
             story_chunk->getStartTime(),
             story_chunk->getEndTime(),
             story_chunk->getEventCount());

    hsize_t size = 0;
    {
        CL_PROFILE_REGION("grapher_hdf5_write");
        StoryChunkWriter chunkWriter(rootDirectory, "story_chunks", "data");
        size = chunkWriter.writeStoryChunk(*story_chunk);
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
