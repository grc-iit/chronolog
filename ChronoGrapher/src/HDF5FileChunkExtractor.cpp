#include "HDF5FileChunkExtractor.h"
#include "StoryChunkWriter.h"

namespace tl = thallium;

namespace chronolog
{
HDF5FileChunkExtractor::HDF5FileChunkExtractor(const std::string &chrono_process_id_card
                                               , const std::string &hdf5_files_root_dir)
                                               : chrono_process_id(chrono_process_id_card)
                                               , rootDirectory(hdf5_files_root_dir)
{}

HDF5FileChunkExtractor::~HDF5FileChunkExtractor()
{
    LOG_INFO("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}

int HDF5FileChunkExtractor::processStoryChunk(StoryChunk *story_chunk)
{
    LOG_INFO("[HDF5FileChunkExtractor] Writing StoryChunk...");
    StoryChunkWriter chunkWriter(rootDirectory, "story_chunks", "data");
    hsize_t size = chunkWriter.writeStoryChunk(*story_chunk);
    int ret = (size == 0) ? CL_ERR_UNKNOWN : CL_SUCCESS;
    if(size == 0)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Error writing StoryChunk to file.");
    }
    else
    {
        LOG_INFO("[HDF5FileChunkExtractor] StoryChunk written to file.");
    }
    LOG_DEBUG("[HDF5FileChunkExtractor] Finished processing StoryChunk.");
    return ret;
}
} // chronolog