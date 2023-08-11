#ifndef POSIX_FILE_CHUNK_EXTRACTOR_H
#define POSIX_FILE_CHUNK_EXTRACTOR_H

#include "chrono_common/chronolog_types.h"
#include "chrono_common/KeeperIdCard.h"
#include "StoryChunkExtractor.h"



namespace chronolog
{

class PosixFileStoryChunkExtractor : public StoryChunkExtractorBase
{


public:
    PosixFileStoryChunkExtractor( );
    //{ }
    ~PosixFileStoryChunkExtractor();
    //{ }

    virtual void processStoryChunk( StoryChunk *);
//    {  }

private:
    std::string rootDirectory;

};



}
#endif
