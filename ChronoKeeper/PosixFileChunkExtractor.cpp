
#include <iostream>
#include <thallium.hpp>

#include "PosixFileChunkExtractor.h"

namespace tl=thallium;

chronolog::PosixFileStoryChunkExtractor::PosixFileStoryChunkExtractor()
{

}
/////////////
chronolog::PosixFileStoryChunkExtractor::~PosixFileStoryChunkExtractor()
{


}
/////////////
void chronolog::PosixFileStoryChunkExtractor::processStoryChunk(chronolog::StoryChunk * storyChunk)
{
    tl::xstream es = tl::xstream::self();
    std::cout <<"PosixFileStoryChunkExtractor::process StoryChunk from ES "
        << es.get_rank() << ", ULT "
        << tl::thread::self_id()<< " StoryChunk {"<< storyChunk->getStoryId()<<":"<< storyChunk->getStartTime()<<'}'<<std::endl;

}

