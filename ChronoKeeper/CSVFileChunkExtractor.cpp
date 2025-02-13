#include <iostream>
#include <fstream>
#include <thallium.hpp>

#include "chronolog_types.h"
#include "chronolog_errcode.h"
#include "KeeperIdCard.h"
#include "CSVFileChunkExtractor.h"

namespace tl = thallium;

chronolog::CSVFileStoryChunkExtractor::CSVFileStoryChunkExtractor(chronolog::KeeperIdCard const &keeper_id_card
                                                                  , std::string const &csv_files_root_dir)
        : keeperIdCard(keeper_id_card), rootDirectory(csv_files_root_dir)
{}

/////////////
chronolog::CSVFileStoryChunkExtractor::~CSVFileStoryChunkExtractor()
{
    LOG_INFO("[CSVFileStoryChunkExtractor] Destructor called. Cleaning up...");
}

/////////////
int chronolog::CSVFileStoryChunkExtractor::processStoryChunk(StoryChunk*story_chunk)
{
    std::ofstream chunk_fstream;

    // chunk_filename: /rootDirectory/storyId.chunkStartTime.keeperIP.port.csv

    std::string chunk_filename(rootDirectory);
    chunk_filename += "/" + std::to_string(story_chunk->getStoryId()) + "." + std::to_string(story_chunk->getStartTime() / 1000000000) + ".";
    keeperIdCard.getRecordingServiceId().get_ip_as_dotted_string(chunk_filename);
    chunk_filename += "." + std::to_string(keeperIdCard.getRecordingServiceId().getPort()) + ".csv";

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
    LOG_DEBUG("[CSVFileStoryChunkExtractor] Finished processing StoryChunk. File={}", chunk_filename);

    return chronolog::CL_SUCCESS;
}

