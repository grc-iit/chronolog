#ifndef CHRONOLOG_STORY_CHUNK_WRITER_H
#define CHRONOLOG_STORY_CHUNK_WRITER_H

#include <string>
#include <memory>
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

    hsize_t writeEvents(std::unique_ptr<H5::H5File> &file, std::vector <LogEventHVL> &data);

    static H5::CompType createEventCompoundType()
    {
        H5::CompType data_type(sizeof(LogEventHVL));
        data_type.insertMember("storyId", HOFFSET(LogEventHVL, storyId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventTime", HOFFSET(LogEventHVL, eventTime), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("clientId", HOFFSET(LogEventHVL, clientId), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("eventIndex", HOFFSET(LogEventHVL, eventIndex), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("logRecord", HOFFSET(LogEventHVL, logRecord), H5::VarLenType(H5::PredType::NATIVE_UINT8));
        return data_type;
    }

private:
    std::string rootDirectory;
    std::string groupName;
    std::string dsetName;
    int numDims;
};
} // chronolog

#endif //CHRONOLOG_STORY_CHUNK_WRITER_H
