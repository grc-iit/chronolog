#include <filesystem>
#include <fstream>
#include <json-c/json.h>
#include <thallium.hpp>

#include <chronolog_types.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <ChunkExtractorCSV.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::StoryChunkExtractorCSV::StoryChunkExtractorCSV(chronolog::ServiceId const& service_id,
                                                          std::string const& csv_archive_dir)
    : serviceId(service_id)
    , outputDirectory(csv_archive_dir)
{
    serviceId.get_ip_as_dotted_string(service_id_string);
}

int chronolog::StoryChunkExtractorCSV::reset(std::string const& new_archive_dir)
{
    outputDirectory = new_archive_dir;
    LOG_INFO("StoryChunkExtractorCSV] Reset success: using csv directory :", outputDirectory);
    return chl::CL_SUCCESS;
}

// json block for CSV Extractor looks like this
//
//  "extractor": {
//                "type": "csv_extractor",
//                "csv_archive_dir": "/tmp/csv_archive"
//               }
//
//////////

int chronolog::StoryChunkExtractorCSV::reset(json_object* json_block)
{
    if( (json_block == nullptr )
      || !json_object_is_type(json_block, json_type_object)
      || (json_object_object_get(json_block, "type") == nullptr)
      || !json_object_is_type(json_object_object_get(json_block, "type"), json_type_string)
      || (std::string("csv_extractor").compare(json_object_get_string(json_object_object_get(json_block,"type"))) != 0)
      )
    {
        outputDirectory = "/tmp";
        LOG_ERROR("StoryChunkExtractorCSV] Reset failure, using csv directory {}", outputDirectory);
        return chl::CL_ERR_INVALID_CONF;    
    }

    if( (json_object_object_get(json_block, "csv_archive_dir") == nullptr)
      || !json_object_is_type(json_object_object_get(json_block, "csv_archive_dir"), json_type_string))
    {
        outputDirectory = "/tmp";
        LOG_ERROR("StoryChunkExtractorCSV] Reset failure, using csv directory {}", outputDirectory);
        return chl::CL_ERR_INVALID_CONF;  
    } 
     
    outputDirectory = json_object_get_string(json_object_object_get(json_block, "csv_archive_dir"));

    // check if archive directory exists and is writable by the extractor process
    if(!std::filesystem::exists(outputDirectory))
    {
        outputDirectory = "/tmp";
        LOG_ERROR("StoryChunkExtractorCSV] Reset failure: csv_archive_dir doesn't exist or not writable; using {}", outputDirectory);
        return chl::CL_ERR_INVALID_CONF;
    }

    LOG_INFO("StoryChunkExtractorCSV] Reset success: using csv directory {}", outputDirectory);
    return chl::CL_SUCCESS;
}

///////////////////

int chronolog::StoryChunkExtractorCSV::process_chunk(StoryChunk* story_chunk)
{

    LOG_DEBUG("[StoryChunkExtractorCSV] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",
              thallium::thread::self_id(),
              story_chunk->getStoryId(),
              story_chunk->getChronicleName(),
              story_chunk->getStoryName(),
              story_chunk->getStartTime(),
              story_chunk->getEndTime(),
              story_chunk->getEventCount());

    // chunk_filename: outputDirectory/storyId.chunkStartTime.serviceIP.port.csv

    std::ofstream chunk_fstream;
    std::string chunk_filename(outputDirectory);
    chunk_filename += "/" + story_chunk->getChronicleName() + "." + story_chunk->getStoryName() + ".";
    serviceId.get_ip_as_dotted_string(chunk_filename);
    chunk_filename += "." + std::to_string(serviceId.getPort()) + "." +
                      std::to_string(story_chunk->getStartTime() / 1000000000) + ".csv";

    // current thread is the only one holding this story_chunk and the only one that's writing to this chunk csv file
    // thus no additional locking is needed ...
    chunk_fstream.open(chunk_filename, std::ofstream::out | std::ofstream::app);
    for(auto event_iter = story_chunk->begin(); event_iter != story_chunk->end(); ++event_iter)
    {
        chronolog::LogEvent const& event = (*event_iter).second;
        chunk_fstream << event << std::endl;
    }
    chunk_fstream.close();

    LOG_DEBUG("[StoryChunkExtractorCSV] Finished processing chunk StoryID={} StartTime={} File={}",
              story_chunk->getStoryId(),
              story_chunk->getStartTime(),
              chunk_filename);

    return chl::CL_SUCCESS;
}
