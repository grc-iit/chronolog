#ifndef CHRONOLOG_STORY_CHUNK_WRITER_H
#define CHRONOLOG_STORY_CHUNK_WRITER_H

#include <string>
#include <utility>
#include <vector>
#include <memory>
#include <H5Cpp.h>
#include <chrono_monitor.h>

#include "StoryChunk.h"
#include "LogEventHVL.h"
#include "StoryChunkHVL.h"

namespace chronolog
{
class StoryChunkWriter
{
public:
    // writer_tag goes into every file name (see ArchiveLayout.h): writerTag()
    // of the grapher's recording group
    StoryChunkWriter(std::string const& root_dir,
                     std::string const& group_name,
                     std::string const& dset_name,
                     std::string writer_tag = writerTag("0"))
        : rootDirectory(root_dir)
        , groupName(group_name)
        , dsetName(dset_name)
        , numDims(1)
        , writerTagValue(std::move(writer_tag)){};

    // "<recording group>.<this process's start time, ns>": together with the
    // per-process sequence, a file name no writer uses twice
    static std::string writerTag(std::string const& recording_group);

    ~StoryChunkWriter() { LOG_DEBUG("[StoryChunkWriter] Destructor called. Cleaning up..."); }

    hsize_t writeStoryChunk(StoryChunkHVL& story_chunk);

    // Writes the chunk under a temporary name and moves it into place under a
    // name never used before. Returns the file size, 0 on failure;
    // published_file, when given, receives the path the file got.
    hsize_t writeStoryChunk(StoryChunk& story_chunk, std::string* published_file = nullptr);

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
    // what both writeStoryChunk overloads do once they have the events
    hsize_t writeWindow(std::string const& chronicle_name,
                        std::string const& story_name,
                        uint64_t start_time,
                        uint64_t incarnation,
                        std::vector<LogEventHVL>& data,
                        std::string* published_file);

    std::string rootDirectory;
    std::string groupName;
    std::string dsetName;
    int numDims;
    std::string writerTagValue;
};
} // namespace chronolog

#endif //CHRONOLOG_STORY_CHUNK_WRITER_H
