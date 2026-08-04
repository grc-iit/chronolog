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
// and re-deriving the name in the caller would duplicate the rotation logic that
// picked it.
struct StoryChunkWriteResult
{
    hsize_t file_size = 0;
    std::string file_name; // as published, empty on failure
    uint32_t seq = 0;      // rotation index: 0 for the base name, n for "...vlen.<n>.h5"
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

    // Builds the file under a temporary name and publishes it atomically, so the
    // Player never sees a partially written archive file and a concurrent publish
    // of the same window rotates rather than overwriting.
    StoryChunkWriteResult writeStoryChunk(StoryChunk& story_chunk);

    // The in-progress name for a chosen final name. Deliberately does not end in
    // ".h5": the Player admits archive files on that extension alone, and the
    // rotation scan in getStoryChunkFileName matches it too, so an in-progress
    // file carrying it would be both readable and countable while incomplete.
    static std::string temporaryFileNameFor(std::string const& final_file_name);

    hsize_t writeEvents(std::unique_ptr<H5::H5File>& file, std::vector<LogEventHVL>& data);

    static H5::CompType createEventCompoundType()
    {
        H5::CompType data_type(sizeof(LogEventHVL));
        data_type.insertMember("storyId", HOFFSET(LogEventHVL, storyId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventTime", HOFFSET(LogEventHVL, eventTime), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("clientId", HOFFSET(LogEventHVL, clientId), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("eventIndex", HOFFSET(LogEventHVL, eventIndex), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("logRecord",
                               HOFFSET(LogEventHVL, logRecord),
                               H5::VarLenType(H5::PredType::NATIVE_UINT8));
        return data_type;
    }

    // base_file_name should be in the format of chronicleName.storyName.startTime.vlen.h5, not including the path
    static std::string getStoryChunkFileName(std::string const& root_dir, std::string const& base_file_name);

    // Rotation index encoded in a published name; 0 when there is none.
    static uint32_t rotationIndexOf(std::string const& file_name);

    // Publishes a completed temporary file under final_path. Returns true on
    // success, false if final_path already exists (or the link fails otherwise) --
    // it never replaces an existing file. Exposed so that property can be tested
    // directly: the collision it guards against happens between choosing a free
    // name and publishing it, a window a caller cannot reproduce from outside.
    static bool publishFile(std::string const& temp_path, std::string const& final_path);

private:
    // How many rotation indices to try before giving up on publishing. Each retry
    // means another stream published the same window in between, so a handful is
    // already far beyond what contention can produce.
    static constexpr int PUBLISH_ATTEMPTS = 8;

    std::string rootDirectory;
    std::string groupName;
    std::string dsetName;
    int numDims;
};
} // namespace chronolog

#endif //CHRONOLOG_STORY_CHUNK_WRITER_H
