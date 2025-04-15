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
    StoryChunkExtractorRDMA(tl::engine &extraction_engine, tl::remote_procedure &drain_to_grapher
                            , tl::provider_handle &service_ph);

    ~StoryChunkExtractorRDMA();

    int processStoryChunk(StoryChunk*story_chunk) override;

private:
//    char serialized_buf[MAX_BULK_MEM_SIZE];
    tl::engine &extraction_engine;
    tl::remote_procedure drain_to_grapher;
    tl::provider_handle service_ph;
};

}


#endif //CHRONOLOG_STORYCHUNKEXTRACTORRDMA_H
