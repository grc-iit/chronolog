#ifndef CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
#define CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H

#include <chrono>
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

// Which of a story's files a destroy deletes. The story can have been
// created again by the time the destroy runs (it waits for extraction to
// drain), and the new story's files sit in the same directory, so only the
// destroyed story's are deleted:
//  - this grapher process's files of pipelines numbered up to
//    incarnation_bound, the number current when the destroy arrived (a
//    pipeline made after it, for the story created again, is numbered
//    higher; see StoryPipeline::currentIncarnation);
//  - every file of an earlier process of this recording group;
//  - another grapher's file only when it was last modified more than
//    HDF5FileChunkExtractor::kForeignFileMargin (120 s) before requested_at.
//    That grapher runs the same destroy for its own files; this covers one
//    that is down. The margin absorbs a difference between the file server's
//    clock and this one.
// The defaults delete every file of the story.
struct ArchiveDestroyScope
{
    uint64_t incarnation_bound = UINT64_MAX;
    std::chrono::system_clock::time_point requested_at = std::chrono::system_clock::time_point::max();
};

class HDF5FileChunkExtractor
{
public:
    HDF5FileChunkExtractor(std::string const& hdf5_archive_dir = "/tmp");

    ~HDF5FileChunkExtractor();

    int process_chunk(StoryChunk*);

    int reset(std::string const& hdf5_archive_dir);
    int reset(json_object*);

    bool is_active() const { return (std::filesystem::exists(rootDirectory)); }

    using DestroyScope = ArchiveDestroyScope;
    // see ArchiveDestroyScope
    static constexpr std::chrono::seconds kForeignFileMargin{120};

    // Delete the persisted HDF5 files of a story that scope covers, in its
    // directory (<archive>/<chronicle>/<story>, see ArchiveLayout.h; the
    // directory stays). Returns the count of deleted files in deleted_count
    // when non-null; returns CL_SUCCESS even if there were none (a destroy on a
    // never-recorded story is not an error).
    int delete_story_files(std::string const& chronicle_name,
                           std::string const& story_name,
                           size_t* deleted_count = nullptr,
                           DestroyScope const& scope = DestroyScope());

    // Delete the persisted HDF5 files of a chronicle, as for a story.
    int delete_chronicle_files(std::string const& chronicle_name,
                               size_t* deleted_count = nullptr,
                               DestroyScope const& scope = DestroyScope());

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
    // The configured root may be a symlink; directories below it may not redirect a destroy.
    int validateArchiveDirectory(std::filesystem::path const& dir) const;
    // whether a destroy with this scope deletes file (see DestroyScope)
    bool coveredByDestroy(std::filesystem::path const& file, DestroyScope const& scope) const;
    // deletes the archive files in one story directory that scope covers
    int deleteStoryDirectory(std::filesystem::path const& dir,
                             std::string const& what,
                             DestroyScope const& scope,
                             size_t& count);

    // appends the deletion of a story, or of the whole chronicle when
    // story_name is null, to the manifest if one is open
    int recordDeletion(std::string const& chronicle_name, std::string const* story_name, DestroyScope const& scope);

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
