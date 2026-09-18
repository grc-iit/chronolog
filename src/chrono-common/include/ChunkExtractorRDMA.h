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
    StoryChunkExtractorRDMA(tl::engine* tl_engine, ServiceId const& service_id = ServiceId());
    StoryChunkExtractorRDMA(StoryChunkExtractorRDMA const& other) = delete;
    StoryChunkExtractorRDMA& operator=(StoryChunkExtractorRDMA const& other) = delete;
    StoryChunkExtractorRDMA(StoryChunkExtractorRDMA&&);
    StoryChunkExtractorRDMA& operator=(StoryChunkExtractorRDMA&& other);


    ~StoryChunkExtractorRDMA();

    tl::engine* get_sender_engine() const { return sender_tl_engine; }
    ServiceId const& get_receiver_service_id() const { return receiver_service_id; }

    // Identity of this keeper's DataStoreAdminService, sent along with every
    // chunk so the grapher knows where to push watermark reports.
    void set_reporter_service_id(ServiceId const& reporter) { reporter_service_id = reporter; }
    ServiceId const& get_reporter_service_id() const { return reporter_service_id; }

    int process_chunk(StoryChunk*);

    int reset(ServiceId const&);
    int reset(json_object*);

    bool is_active() const { return (nullptr != rdma_sender); }

private:
    tl::engine* sender_tl_engine;  // local tl::engine
    ServiceId receiver_service_id; // receiving ServiceId
    ServiceId reporter_service_id; // this sender's watermark-report target
    RDMATransferAgent* rdma_sender;

    void restart_rdma_sender(ServiceId const&);
};


} // namespace chronolog
#endif
