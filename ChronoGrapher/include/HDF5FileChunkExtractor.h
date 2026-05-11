#ifndef CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
#define CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H

#include <filesystem>
#include <string>

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

    void stash_to_outage_buffer(StoryChunk* chunk);
    void flush_outage_buffer();

private:
    std::string rootDirectory;
};

} // namespace chronolog

#endif //CHRONOLOG_HDF5_FILE_CHUNK_EXTRACTOR_H
