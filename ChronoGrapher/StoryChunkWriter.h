#ifndef CHRONOLOG_STORY_CHUNK_WRITER_H
#define CHRONOLOG_STORY_CHUNK_WRITER_H

#include <string>
#include <H5Cpp.h>
#include "chrono_monitor.h"
#include "StoryChunk.h"

namespace chronolog
{
class StoryChunkWriter
{
public:
    StoryChunkWriter(std::string const &root_dir, std::string const &group_name, std::string const &dset_name)
            : rootDirectory(root_dir), groupName(group_name), dsetName(dset_name), numDims(1)
    {};

    ~StoryChunkWriter()
    {
        LOG_INFO("[StoryChunkWriter] Destructor called. Cleaning up...");
    }

    hsize_t writeStoryChunk(StoryChunkHVL &story_chunk);

    hsize_t writeStoryChunk(StoryChunk &story_chunk);

    hsize_t writeEvents(H5::H5File *file, std::vector <LogEventHVL> &data);

    H5::CompType createEventCompoundType();

private:
    std::string rootDirectory;
    std::string groupName;
    std::string dsetName;
    int numDims;
};
} // chronolog

#endif //CHRONOLOG_STORY_CHUNK_WRITER_H
