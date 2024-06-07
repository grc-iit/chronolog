#ifndef CSV_FILE_CHUNK_EXTRACTOR_H
#define CSV_FILE_CHUNK_EXTRACTOR_H

#include "chronolog_types.h"
#include "StoryChunkExtractor.h"


namespace chronolog
{

class CSVFileStoryChunkExtractor: public StoryChunkExtractorBase
{

public:
    CSVFileStoryChunkExtractor(std::string const & chrono_process_id_card, std::string const &csv_files_root_dir);

    ~CSVFileStoryChunkExtractor();

    virtual int processStoryChunk(StoryChunk*);

private:
    std::string chrono_process_id;
    std::string rootDirectory;


};


}
#endif
