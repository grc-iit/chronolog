#include <iostream>
#include <fstream>
#include <thallium.hpp>

#include <chronolog_types.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <ChunkExtractorCSV.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::StoryChunkExtractorCSV::StoryChunkExtractorCSV(chronolog::ServiceId const & service_id
                                                                  , std::string const & csv_files_dir)
        : serviceId(service_id)
        , outputDirectory(csv_files_dir)
{}

/////////////
chl::StoryChunk * chronolog::StoryChunkExtractorCSV::process_chunk(StoryChunk*story_chunk)
{
    std::ofstream chunk_fstream;

    // chunk_filename: outputDirectory/storyId.chunkStartTime.serviceIP.port.csv

    std::string chunk_filename(outputDirectory);
    chunk_filename += "/" + story_chunk->getChronicleName() + "."+ story_chunk->getStoryName() + "."; 
    serviceId.get_ip_as_dotted_string(chunk_filename);
    chunk_filename += "." + std::to_string(serviceId.getPort()) + "." + std::to_string(story_chunk->getStartTime()/1000000000) + ".csv";

    tl::xstream es = tl::xstream::self();
    LOG_INFO("[CSVFileStoryChunkExtractor] Processing StoryChunk: ES={}, ULT={}, StoryID={}, StartTime={}", es.get_rank()
         , tl::thread::self_id(), story_chunk->getStoryId(), story_chunk->getStartTime());

    // current thread if the only one that has this storyChunk and the only one that's writing to this chunk csv file 
    // thus no additional locking is needed ... 
    chunk_fstream.open(chunk_filename, std::ofstream::out|std::ofstream::app);
    for(auto event_iter = story_chunk->begin(); event_iter != story_chunk->end(); ++event_iter)
    {
        chronolog::LogEvent const &event = (*event_iter).second;
        chunk_fstream << event << std::endl;
    }
    chunk_fstream.close();
    LOG_DEBUG("[StoryChunkExtractorCSV] Finished processing StoryChunk. File={}", chunk_filename);

    return story_chunk;
}
