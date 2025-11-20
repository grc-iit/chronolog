#include <iostream>
#include <fstream>
#include <thallium.hpp>

#include <chronolog_types.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <ChunkExtractorCSV.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::StoryChunkExtractorCSV::StoryChunkExtractorCSV(chronolog::ServiceId const& service_id,
                                                          std::string const& csv_files_dir)
    : serviceId(service_id)
    , outputDirectory(csv_files_dir)
{}

//////////
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
