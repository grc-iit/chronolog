// Writing an archive the way a grapher does, for the player's archive tests:
// the window's file through StoryChunkWriter, then its record in the grapher's
// manifest log.

#ifndef CHRONOLOG_ARCHIVE_TEST_SUPPORT_H
#define CHRONOLOG_ARCHIVE_TEST_SUPPORT_H

#include <filesystem>
#include <string>

#include <ArchiveManifest.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>

namespace chronolog::test
{

// Publishes the chunk and returns the path of its file, empty on failure.
inline std::string
publishWindow(std::filesystem::path const& archive_dir, StoryChunk& chunk, std::string const& writer = "1")
{
    StoryChunkWriter file_writer(archive_dir.string(), "story_chunks", "data");
    std::string file;
    if(file_writer.writeStoryChunk(chunk, &file) == 0)
    {
        return {};
    }
    ArchiveManifestWriter manifest(archive_dir.string(), writer);
    if(manifest.open() != CL_SUCCESS)
    {
        return {};
    }
    ArchiveManifestRecord record;
    record.op = ArchiveManifestRecord::Op::PUBLISH;
    record.chronicle = chunk.getChronicleName();
    record.story = chunk.getStoryName();
    record.file = std::filesystem::path(file).lexically_relative(archive_dir).string();
    record.start = chunk.getStartTime();
    record.end = chunk.getEndTime();
    record.events = chunk.getEventCount();
    return (manifest.append(record) == CL_SUCCESS) ? file : std::string();
}

// Records the deletion of a story, or of the whole chronicle when story is
// empty, in the writer's log. Deleting the files is up to the caller.
inline bool recordDeletion(std::filesystem::path const& archive_dir,
                           std::string const& chronicle,
                           std::string const& story,
                           std::string const& writer = "1",
                           uint64_t writer_start = 0,
                           uint64_t incarnation_bound = UINT64_MAX)
{
    ArchiveManifestWriter manifest(archive_dir.string(), writer);
    if(manifest.open() != CL_SUCCESS)
    {
        return false;
    }
    ArchiveManifestRecord record;
    record.op = ArchiveManifestRecord::Op::DELETE;
    record.chronicle = chronicle;
    record.story = story;
    record.whole_chronicle = story.empty();
    record.writer_start = writer_start;
    record.incarnation_bound = incarnation_bound;
    return manifest.append(record) == CL_SUCCESS;
}

} // namespace chronolog::test

#endif // CHRONOLOG_ARCHIVE_TEST_SUPPORT_H
