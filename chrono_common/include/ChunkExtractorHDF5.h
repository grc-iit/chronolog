#ifndef CHUNK_EXTRACTOR_HDF5_H
#define CHUNK_EXTRACTOR_HDF5_H

#include <chronolog_types.h>
#include <ServiceId.h>

namespace chronolog
{

class StoryChunk;

class StoryChunkExtractorHDF5
{

public:
    StoryChunkExtractorHDF5(ServiceId const & service_id, std::string const & target_dir);

    StoryChunkExtractorHDF5() = default;

    int process_chunk(StoryChunk*);

private:
    ServiceId serviceId;
    std::string targetDirectory;


};


}
#endif
