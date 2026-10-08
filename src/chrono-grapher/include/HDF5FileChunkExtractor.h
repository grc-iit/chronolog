#ifndef CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
#define CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct json_object;

namespace chronolog
{

class ArchiveManifestWriter;
class StoryChunk;
class StoryWatermarkRegistry;

class HDF5FileChunkExtractor
{
public:
    HDF5FileChunkExtractor(std::string const& hdf5_archive_dir = "/tmp");

    ~HDF5FileChunkExtractor();

    int process_chunk(StoryChunk*);

    int reset(std::string const& hdf5_archive_dir);
    int reset(json_object*);

    bool is_active() const { return (std::filesystem::exists(rootDirectory)); }

    // Delete the persisted HDF5 files of a story, in its
    // directory (<archive>/<chronicle>/<story>, see ArchiveLayout.h; the
    // directory stays). Returns the count of deleted files in deleted_count
    // when non-null; returns CL_SUCCESS even if there were none (a destroy on a
    // never-recorded story is not an error).
    int delete_story_files(std::string const& chronicle_name,
                           std::string const& story_name,
                           size_t* deleted_count = nullptr);

    // Delete the persisted HDF5 files of a chronicle, as for a story.
    int delete_chronicle_files(std::string const& chronicle_name, size_t* deleted_count = nullptr);

    // Report each successfully written merged window to the registry so the
    // per-story persisted watermark W can advance. Raw non-owning pointer:
    // extractors are moved into the extraction chain's std::variant vector, so
    // the pointer must survive moves. Optional — nullptr disables reporting.
    void attachWatermarkRegistry(StoryWatermarkRegistry* registry) { watermarkRegistry = registry; }

    // Opens this grapher's log in the archive manifest (see ArchiveManifest.h)
    // under writer_id, which must stay the same across restarts. From then on
    // a window counts as written only once its record is appended, and a
    // deletion is recorded too. Call after reset().
    int openArchiveManifest(std::string const& writer_id);

private:
    // deletes the archive files in one story directory
    int deleteStoryDirectory(std::filesystem::path const& dir, std::string const& what, size_t& count);

    // appends the deletion of a story, or of the whole chronicle when
    // story_name is null, to the manifest if one is open
    int recordDeletion(std::string const& chronicle_name, std::string const* story_name);

    std::string rootDirectory;
    // the writer part of the names of the files this extractor publishes (see
    // ArchiveLayout.h); its recording group once the manifest is open
    std::string fileWriterTag;
    StoryWatermarkRegistry* watermarkRegistry = nullptr;
    // shared: the extractor is copied into the extraction chain's variant
    std::shared_ptr<ArchiveManifestWriter> archiveManifest;
};

} // namespace chronolog

#endif //CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
