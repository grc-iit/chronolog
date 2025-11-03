#ifndef CHUNK_EXTRACTOR_CSV_H
#define CHUNK_EXTRACTOR_CSV_H

#include <chronolog_types.h>
#include <ServiceId.h>

#include "StoryChunk.h"


namespace chronolog
{

class StoryChunkExtractorCSV
{

public:
    StoryChunkExtractorCSV(ServiceId const & service_id, std::string const &csv_files_dir);

    StoryChunkExtractorCSV() = default;

    StoryChunk * process_chunk(StoryChunk*);

private:
    ServiceId serviceId;
    std::string outputDirectory;


};


}
#endif
