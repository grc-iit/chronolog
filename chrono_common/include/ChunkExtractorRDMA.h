#ifndef CHUNK_EXTRACTOR_RDMA_H
#define CHUNK_EXTRACTOR_RDMA_H

#include <json-c/json.h>
#include <thallium.hpp>

#include <chronolog_types.h>
#include <ServiceId.h>

namespace tl = thallium;

namespace chronolog
{

class StoryChunk;
class RDMATransferAgent;

class StoryChunkExtractorRDMA
{

public:
    StoryChunkExtractorRDMA(tl::engine& tl_engine, ServiceId const& service_id);
    StoryChunkExtractorRDMA(StoryChunkExtractorRDMA const& other);
    StoryChunkExtractorRDMA& operator=(StoryChunkExtractorRDMA const& other);

    ~StoryChunkExtractorRDMA();

    tl::engine& get_sender_engine() const { return sender_tl_engine; }
    ServiceId const& get_receiver_service_id() const { return receiver_service_id; }
    int process_chunk(StoryChunk*);

    int reset(ServiceId const&);
    int reset(json_object*);

    bool is_active() const
    { return (nullptr == rdma_sender); }

private:
    tl::engine& sender_tl_engine;  // local tl::engine
    ServiceId receiver_service_id; // receiving ServiceId
    RDMATransferAgent* rdma_sender;

    void restart_rdma_sender(ServiceId const &);
};


} // namespace chronolog
#endif
