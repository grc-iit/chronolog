#ifndef CHRONOLOG_STORY_CHUNK_WRITER_H
#define CHRONOLOG_STORY_CHUNK_WRITER_H

#include <string>
#include <memory>
#include <H5Cpp.h>
#include <chrono_monitor.h>

#include "StoryChunk.h"
#include "LogEventHVL.h"
#include "StoryChunkHVL.h"

namespace chronolog
{

// What a publish produced. file_size doubles as the success flag (0 == nothing
// was written), preserving the caller's existing check; file_name and seq exist
// because the archive manifest has to record the file it will later have to find,
// and re-deriving the name in the caller would duplicate the numbering logic that
// picked it.
struct StoryChunkWriteResult
{
    hsize_t file_size = 0;
    std::string file_name; // as published, empty on failure
    uint32_t seq = 0;      // 0 for the window's own name, n for "...vlen.<n>.h5"
};

class StoryChunkWriter
{
public:
    StoryChunkWriter(std::string const& root_dir, std::string const& group_name, std::string const& dset_name)
        : rootDirectory(root_dir)
        , groupName(group_name)
        , dsetName(dset_name)
        , numDims(1){};

    ~StoryChunkWriter() { LOG_DEBUG("[StoryChunkWriter] Destructor called. Cleaning up..."); }

    hsize_t writeStoryChunk(StoryChunkHVL& story_chunk);

    // Writes the window under a temporary name and moves it into place under the
    // first free name (see publishFile in StoryChunkWriter.cpp), and reports which
    // name it took. An empty chunk writes nothing and returns file_size 0.
    StoryChunkWriteResult writeStoryChunk(StoryChunk& story_chunk);

    // The number a published name carries: 0 for "<...>.vlen.h5", n for
    // "<...>.vlen.<n>.h5".
    static uint32_t rotationIndexOf(std::string const& file_name);

    hsize_t writeEvents(std::unique_ptr<H5::H5File>& file, std::vector<LogEventHVL>& data);

    static H5::CompType createEventCompoundType()
    {
        H5::CompType data_type(sizeof(LogEventHVL));
        data_type.insertMember("storyId", HOFFSET(LogEventHVL, storyId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventTime", HOFFSET(LogEventHVL, eventTime), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("clientId", HOFFSET(LogEventHVL, clientId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventIndex", HOFFSET(LogEventHVL, eventIndex), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("logRecord",
                               HOFFSET(LogEventHVL, logRecord),
                               H5::VarLenType(H5::PredType::NATIVE_UINT8));
        return data_type;
    }

    // The record layout of files written before client ids were widened to 64
    // bits: clientId was a 32-bit member at offset 16, making a 40-byte record.
    // Readers accept it and let HDF5 widen the id; its upper 32 bits are gone.
    static H5::CompType createLegacyEventCompoundType()
    {
        H5::CompType data_type(static_cast<size_t>(40));
        data_type.insertMember("storyId", 0, H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventTime", 8, H5::PredType::NATIVE_UINT64);
        data_type.insertMember("clientId", 16, H5::PredType::NATIVE_UINT32);
        data_type.insertMember("eventIndex", 20, H5::PredType::NATIVE_UINT32);
        data_type.insertMember("logRecord", 24, H5::VarLenType(H5::PredType::NATIVE_UINT8));
        return data_type;
    }

private:
    std::string rootDirectory;
    std::string groupName;
    std::string dsetName;
    int numDims;
};
} // namespace chronolog

#endif //CHRONOLOG_STORY_CHUNK_WRITER_H
