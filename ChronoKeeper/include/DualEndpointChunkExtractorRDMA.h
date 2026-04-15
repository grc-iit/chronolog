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
    DualEndpointChunkExtractorRDMA( tl::engine & tl_engine, ServiceId const & receiver_player, ServiceId const& receiver_grapher);
    DualEndpointChunkExtractorRDMA( DualEndpointChunkExtractorRDMA const & other);
    DualEndpointChunkExtractorRDMA & operator=( DualEndpointChunkExtractorRDMA const & other); 

    ~DualEndpointChunkExtractorRDMA();

    tl::engine & get_sender_engine() const { return sender_tl_engine;}
    ServiceId const& get_player_receiver_id() const { return player_receiver_service_id;}
    ServiceId const& get_grapher_receiver_id() const { return player_receiver_service_id;}
    
    int process_chunk(StoryChunk*);

private:
    tl::engine & sender_tl_engine;         // local tl::engine
    ServiceId player_receiver_service_id;          // ChronoGrapher receiving ServiceId
    ServiceId grapher_receiver_service_id;          // ChronoPlayer receiving ServiceId
    RDMATransferAgent * rdma_sender_for_player;
    RDMATransferAgent * rdma_sender_for_grapher;
};


}
#endif
