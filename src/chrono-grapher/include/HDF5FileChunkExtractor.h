#ifndef CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
#define CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H

#include <filesystem>
#include <string>
#include <vector>

#include <ArchiveManifest.h>

struct json_object;

namespace chronolog
{

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

    // Delete every persisted HDF5 file belonging to a story
    // (<chronicle>.<story>.*.vlen.h5 in rootDirectory). Returns the count of
    // deleted files in deleted_count when non-null; returns CL_SUCCESS even if
    // no files matched (a destroy on a never-recorded story is not an error).
    int delete_story_files(std::string const& chronicle_name,
                           std::string const& story_name,
                           size_t* deleted_count = nullptr);

    // Delete every persisted HDF5 file belonging to a chronicle
    // (<chronicle>.*.vlen.h5 in rootDirectory).
    int delete_chronicle_files(std::string const& chronicle_name, size_t* deleted_count = nullptr);

    // Report each successfully written merged window to the registry so the
    // per-story persisted watermark W can advance. Raw non-owning pointer:
    // extractors are moved into the extraction chain's std::variant vector, so
    // the pointer must survive moves. Optional — nullptr disables reporting.
    void attachWatermarkRegistry(StoryWatermarkRegistry* registry) { watermarkRegistry = registry; }

    // Record every publish outcome (and every deletion) in the archive manifest,
    // which is what a restarted Grapher replays to recover W and what the Player
    // reads instead of scanning the archive tree. Same non-owning-pointer contract
    // as the registry above; optional -- nullptr disables recording.
    void attachArchiveManifest(ArchiveManifest* manifest) { archiveManifest = manifest; }

    // The archive root an "hdf5_extractor" conf block points at, or "" if the
    // block is absent or malformed. ChronoGrapher needs it to construct the
    // manifest before the extraction module, without duplicating the parsing.
    static std::string archiveDirectoryFromConf(json_object* json_block);

private:
    // Appends one record describing what just happened to a chunk. No-op when no
    // manifest is attached.
    void
    recordInManifest(StoryChunk const* story_chunk, ManifestState state, std::string const& file_path, uint32_t seq);

    // Appends a superseding record for each file a delete removed. story_name is
    // empty for a chronicle-wide delete, where the caller does not know it per
    // file; supersession keys on the file name, which is always recorded.
    void recordDeletions(std::string const& chronicle_name,
                         std::string const& story_name,
                         std::vector<std::string> const& deleted_files);

    std::string rootDirectory;
    StoryWatermarkRegistry* watermarkRegistry = nullptr;
    ArchiveManifest* archiveManifest = nullptr;
};

} // namespace chronolog

#endif //CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
