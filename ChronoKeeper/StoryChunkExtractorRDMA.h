#ifndef CHRONOLOG_STORYCHUNKEXTRACTORRDMA_H
#define CHRONOLOG_STORYCHUNKEXTRACTORRDMA_H

#include "chronolog_types.h"
#include "StoryChunkExtractor.h"
#include "ConfigurationManager.h"

namespace tl = thallium;

namespace chronolog
{

class StoryChunkExtractorRDMA: public StoryChunkExtractorBase
{
public:
    StoryChunkExtractorRDMA(tl::engine &extraction_engine, const ChronoLog::ConfigurationManager &confManager);

    ~StoryChunkExtractorRDMA();

    int processStoryChunk(StoryChunk*story_chunk) override;

private:
    size_t serializeStoryChunkWithCereal(StoryChunk*story_chunk, char*mem_ptr);

    tl::engine &extraction_engine;
    tl::remote_procedure drain_to_grapher;
    uint16_t grapher_service_id{};
    tl::provider_handle service_ph;
    ChronoLog::ConfigurationManager confManager;
};

}


#endif //CHRONOLOG_STORYCHUNKEXTRACTORRDMA_H
