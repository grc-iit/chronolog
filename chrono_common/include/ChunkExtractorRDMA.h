#ifndef CHUNK_EXTRACTOR_RDMA_H
#define CHUNK_EXTRACTOR_RDMA_H

#include <chronolog_types.h>
#include <ServiceId.h>

namespace chronolog
{

class StoryChunk;
class RDMATransferAgent;

class StoryChunkExtractorRDMA
{

public:
    StoryChunkExtractorRDMA( tl::engine & tl_engine, ServiceId const & service_id);

    ~StoryChunkExtractorRDMA();

    int process_chunk(StoryChunk*);

private:
    tl::engine & sender_tl_engine;         // local tl::engine
    RDMATransferAgent * rdma_sender;
};


}
#endif
