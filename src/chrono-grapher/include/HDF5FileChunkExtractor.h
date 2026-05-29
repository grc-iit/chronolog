#ifndef CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
#define CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H

#include <filesystem>
#include <string>

struct json_object;

namespace chronolog
{

class StoryChunk;

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

private:
    std::string rootDirectory;
};

} // namespace chronolog

#endif //CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
