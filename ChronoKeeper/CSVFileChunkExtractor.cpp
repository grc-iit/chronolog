
#include <iostream>
#include <fstream>
#include <thallium.hpp>

#include "chronolog_types.h"
#include "KeeperIdCard.h"
#include "CSVFileChunkExtractor.h"

namespace tl=thallium;

chronolog::CSVFileStoryChunkExtractor::CSVFileStoryChunkExtractor( chronolog::KeeperIdCard const& keeper_id_card
            , std::string const& csv_files_root_dir)
        : keeperIdCard(keeper_id_card)
        , rootDirectory(csv_files_root_dir)
{

}
/////////////
chronolog::CSVFileStoryChunkExtractor::~CSVFileStoryChunkExtractor()
{

}
/////////////
void chronolog::CSVFileStoryChunkExtractor::processStoryChunk(chronolog::StoryChunk * story_chunk)
{
    std::ofstream chunk_fstream;
    std::string chunk_filename(rootDirectory);
    keeperIdCard.getIPasDottedString(chunk_filename); 
    chunk_filename += "."+std::to_string(story_chunk->getStoryId()) + "."+ std::to_string(story_chunk->getStartTime()/1000000000)+".csv";
    
    tl::xstream es = tl::xstream::self();
    std::cout <<"CSVFileStoryChunkExtractor::process  ES " << es.get_rank() << ", ULT " << tl::thread::self_id()
        << " StoryChunk {"<< story_chunk->getStoryId()<<":"<< story_chunk->getStartTime()<<'}'
        << " file {"<< chunk_filename<<'}'<<std::endl;    

    // current thread if the only one that has this storyChunk and the only one that's writing to this chunk csv file 
    // thus no additional locking is needed ... 
    chunk_fstream.open( chunk_filename, std::ofstream::out|std::ofstream::app);
    for( auto event_iter = story_chunk->begin(); event_iter != story_chunk->end(); ++event_iter)
    {
        chronolog::LogEvent const& event = (*event_iter).second;
        chunk_fstream << event << std::endl;
    }
    chunk_fstream.close();
}

