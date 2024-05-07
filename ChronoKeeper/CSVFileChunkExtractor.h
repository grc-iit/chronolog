#ifndef CSV_FILE_CHUNK_EXTRACTOR_H
#define CSV_FILE_CHUNK_EXTRACTOR_H

#include "chronolog_types.h"
#include "KeeperIdCard.h"
#include "StoryChunkExtractor.h"


namespace chronolog
{

class CSVFileStoryChunkExtractor: public StoryChunkExtractorBase
{

public:
    CSVFileStoryChunkExtractor(KeeperIdCard const &keeper_id_card, std::string const &csv_files_root_dir);

    ~CSVFileStoryChunkExtractor();

    virtual int processStoryChunk(StoryChunk*);

private:
    KeeperIdCard keeperIdCard;
    std::string rootDirectory;


};


}
#endif
