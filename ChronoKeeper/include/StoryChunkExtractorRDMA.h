#ifndef CHRONOLOG_STORYCHUNKEXTRACTORRDMA_H
#define CHRONOLOG_STORYCHUNKEXTRACTORRDMA_H

#include <thallium.hpp>

#include <chronolog_types.h>
#include <ConfigurationManager.h>

#include "StoryChunk.h"
#include "StoryChunkExtractor.h"

namespace tl = thallium;

namespace chronolog
{

class StoryChunkExtractorRDMA: public StoryChunkExtractorBase
{
public:
    StoryChunkExtractorRDMA(tl::engine& extraction_engine,
                            tl::remote_procedure& drain_to_grapher,
                            tl::provider_handle& service_ph);

    ~StoryChunkExtractorRDMA();

    int processStoryChunk(StoryChunk* story_chunk) override;

private:
    //    char serialized_buf[MAX_BULK_MEM_SIZE];
    tl::engine& extraction_engine;
    tl::remote_procedure drain_to_grapher;
    tl::provider_handle service_ph;
};

} // namespace chronolog


#endif //CHRONOLOG_STORYCHUNKEXTRACTORRDMA_H
