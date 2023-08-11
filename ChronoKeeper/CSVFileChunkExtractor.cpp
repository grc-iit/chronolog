
#include <iostream>
#include <fstream>
#include <thallium.hpp>

#include "chronolog_types.h"
#include "CSVFileChunkExtractor.h"

namespace tl=thallium;

chronolog::PosixFileStoryChunkExtractor::PosixFileStoryChunkExtractor()
{

}
/////////////
chronolog::PosixFileStoryChunkExtractor::~PosixFileStoryChunkExtractor()
{


}
/////////////
void chronolog::PosixFileStoryChunkExtractor::processStoryChunk(chronolog::StoryChunk * story_chunk)
{
    tl::xstream es = tl::xstream::self();
    std::cout <<"PosixFileStoryChunkExtractor::process StoryChunk from ES "
        << es.get_rank() << ", ULT "
        << tl::thread::self_id()<< " StoryChunk {"<< story_chunk->getStoryId()<<":"<< story_chunk->getStartTime()<<'}'<<std::endl;

    std::ofstream chunk_fstream;
    std::string chunk_filename = /*keeperIdAsstring+ */ 
            std::to_string(story_chunk->getStoryId()) + std::to_string(story_chunk->getStartTime());
    chunk_fstream.open( chunk_filename, std::ofstream::out|std::ofstream::app);
    for( auto event_iter = story_chunk->begin(); event_iter != story_chunk->end(); ++event_iter)
    {
        chronolog::LogEvent const& event = (*event_iter).second;
        chunk_fstream << event << std::endl;
    }
    chunk_fstream.close();
}

