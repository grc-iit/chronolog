#ifndef CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
#define CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H

#include "StoryChunkExtractor.h"

namespace chronolog
{

class HDF5FileChunkExtractor: public StoryChunkExtractorBase
{
public:
    HDF5FileChunkExtractor(std::string const &chrono_process_id_card, std::string const &hdf5_files_root_dir);

    ~HDF5FileChunkExtractor();

    virtual int processStoryChunk(StoryChunk *);

private:
    std::string chrono_process_id;
    std::string rootDirectory;
};

} // chronolog

#endif //CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
