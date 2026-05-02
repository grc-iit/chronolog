#include <filesystem>
#include <json-c/json.h>
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
    if( (json_block == nullptr )
      || !json_object_is_type(json_block, json_type_object)
      || (json_object_object_get(json_block, "type") == nullptr)
      || !json_object_is_type(json_object_object_get(json_block, "type"), json_type_string)
      || (std::string("hdf5_extractor").compare(json_object_get_string(json_object_object_get(json_block,"type"))) != 0)
      )
    {
        rootDirectory = "/tmp";
        LOG_ERROR("HDF5FileChunkExtractor] Reset failure: invalid json_conf; using {}", rootDirectory);
        return chl::CL_ERR_INVALID_CONF;    
    }

    if( (json_object_object_get(json_block, "hdf5_archive_dir") == nullptr)
      || !json_object_is_type(json_object_object_get(json_block, "hdf5_archive_dir"), json_type_string))
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
        LOG_ERROR("HDF5FileChunkExtractor] Reset failure: hdf5_archive_dir doesn't exist or not writable; using {} ", rootDirectory);
        return chl::CL_ERR_INVALID_CONF;  
    }
     
    LOG_INFO("HDF5FileChunkExtractor] Reset success: using {}", rootDirectory);
    return chl::CL_SUCCESS;
}

//////


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
