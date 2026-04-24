#ifndef CHUNK_EXTRACTOR_CSV_H
#define CHUNK_EXTRACTOR_CSV_H

#include <json-c/json.h>

#include <chronolog_types.h>
#include <ServiceId.h>

#include "StoryChunk.h"


namespace chronolog
{

class StoryChunkExtractorCSV
{

public:
    StoryChunkExtractorCSV(ServiceId const& service_id, std::string const& csv_archive_dir="/tmp");

    int reset(std::string const& csv_archive_dir);
    int reset(json_object*);

    StoryChunkExtractorCSV() = default;

    int process_chunk(StoryChunk*);

private:
    ServiceId serviceId;
    std::string outputDirectory;
    std::string service_id_string;
};


} // namespace chronolog
#endif
