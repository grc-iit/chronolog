#ifndef DUAL_CHUNK_EXTRACTOR_RDMA_H
#define DUAL_CHUNK_EXTRACTOR_RDMA_H

#include <chronolog_types.h>
#include <ServiceId.h>

namespace tl = thallium;

namespace chronolog
{

class StoryChunk;
class RDMATransferAgent;

class DualEndpointChunkExtractorRDMA
{

public:
    DualEndpointChunkExtractorRDMA(tl::engine& tl_engine,
                                   ServiceId const& receiver_player = ServiceId(),
                                   ServiceId const& receiver_grapher = ServiceId());
    DualEndpointChunkExtractorRDMA(DualEndpointChunkExtractorRDMA const& other);
    DualEndpointChunkExtractorRDMA& operator=(DualEndpointChunkExtractorRDMA const& other);

    ~DualEndpointChunkExtractorRDMA();

    tl::engine& get_sender_engine() const { return sender_tl_engine; }
    ServiceId const& get_player_receiver_id() const { return player_receiver_service_id; }
    ServiceId const& get_grapher_receiver_id() const { return player_receiver_service_id; }

    int process_chunk(StoryChunk*);

    int reset_player_endpoint(ServiceId const& receiver_player);
    int reset_grapher_endpoint(ServiceId const& receiver_grapher);
    int reset(json_object*);

    bool is_active() const { return (nullptr != rdma_sender_for_grapher) && (nullptr != rdma_sender_for_player); }

private:
    tl::engine& sender_tl_engine;          // local tl::engine
    ServiceId player_receiver_service_id;  // ChronoGrapher receiving ServiceId
    ServiceId grapher_receiver_service_id; // ChronoPlayer receiving ServiceId
    RDMATransferAgent* rdma_sender_for_player;
    RDMATransferAgent* rdma_sender_for_grapher;

    void restart_rdma_sender_for_grapher(ServiceId const&);
    void restart_rdma_sender_for_player(ServiceId const&);
};


} // namespace chronolog
#endif
