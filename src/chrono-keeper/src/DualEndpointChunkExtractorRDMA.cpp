#include <cereal/archives/binary.hpp>
#include <json-c/json.h>
//#include <thallium/serialization/stl/vector.hpp>
#include <thallium.hpp>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <ConfigurationBlocks.h>
#include <StoryChunk.h>
#include <RDMATransferAgent.h>
#include <DualEndpointChunkExtractorRDMA.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::DualEndpointChunkExtractorRDMA::DualEndpointChunkExtractorRDMA(
        tl::engine* tl_engine,
        chronolog::ServiceId const& player_receiving_service_id,
        chronolog::ServiceId const& grapher_receiving_service_id)
    : sender_tl_engine(tl_engine)
    , player_receiver_service_id(player_receiving_service_id)
    , grapher_receiver_service_id(grapher_receiving_service_id)
    , rdma_sender_for_player(nullptr)
    , rdma_sender_for_grapher(nullptr)
{
    restart_rdma_sender_for_grapher(grapher_receiver_service_id);

    restart_rdma_sender_for_player(player_receiver_service_id);
}

/////////////

void chronolog::DualEndpointChunkExtractorRDMA::restart_rdma_sender_for_grapher(
        chl::ServiceId const& receiver_service_id)
{
    if(rdma_sender_for_grapher != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] assingment : deleting receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
        delete rdma_sender_for_grapher;
    }

    grapher_receiver_service_id = receiver_service_id;

    if(!grapher_receiver_service_id.is_valid())
    {
        return;
    }

    try
    {
        rdma_sender_for_grapher =
                RDMATransferAgent::CreateRDMATransferAgent(*sender_tl_engine, grapher_receiver_service_id);
        LOG_TRACE("[DualEndpointChunkExtractor] created rdma_sender for grpaher_receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointChunkExtractor] failed to create rdma_sender for grapher_receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
        rdma_sender_for_grapher = nullptr;
    }
}

/////////////

void chronolog::DualEndpointChunkExtractorRDMA::restart_rdma_sender_for_player(
        chl::ServiceId const& receiver_service_id)
{
    if(rdma_sender_for_player != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] assingment : deleting receiver {} ",
                  chl::to_string(player_receiver_service_id));
        delete rdma_sender_for_player;
    }

    player_receiver_service_id = receiver_service_id;

    if(!player_receiver_service_id.is_valid())
    {
        return;
    }

    try
    {
        rdma_sender_for_player =
                RDMATransferAgent::CreateRDMATransferAgent(*sender_tl_engine, player_receiver_service_id);
        LOG_TRACE("[DualEndpointChunkExtractor] Constructor created rdma_sender for player_receiver {} ",
                  chl::to_string(player_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointChunkExtractor] Constructor : failed to create rdma_sender for player_receiver {} ",
                  chl::to_string(player_receiver_service_id));
        rdma_sender_for_player = nullptr;
    }
}


chronolog::DualEndpointChunkExtractorRDMA::DualEndpointChunkExtractorRDMA(DualEndpointChunkExtractorRDMA&& other)
{
    LOG_TRACE("[DualEndpointExtractorRDMA] Move Constructor ");
    if(this == &other)
    {
        return;
    }

    sender_tl_engine = other.get_sender_engine();
    player_receiver_service_id = other.get_player_receiver_id();
    grapher_receiver_service_id = other.get_grapher_receiver_id();
    reporter_service_id = other.get_reporter_service_id();
    rdma_sender_for_player = other.rdma_sender_for_player;
    rdma_sender_for_grapher = other.rdma_sender_for_grapher;

    other.sender_tl_engine = nullptr;
    other.rdma_sender_for_player = nullptr;
    other.rdma_sender_for_grapher = nullptr;
}

chl::DualEndpointChunkExtractorRDMA&
chronolog::DualEndpointChunkExtractorRDMA::operator=(DualEndpointChunkExtractorRDMA&& other)
{
    if(this == &other)
    {
        return *this;
    }

    if(rdma_sender_for_player != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] move operator: deleting receiver {} ",
                  chl::to_string(player_receiver_service_id));
        delete rdma_sender_for_player;
    }
    if(rdma_sender_for_grapher != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] move operator: deleting receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
        delete rdma_sender_for_grapher;
    }

    sender_tl_engine = other.get_sender_engine();
    player_receiver_service_id = other.get_player_receiver_id();
    grapher_receiver_service_id = other.get_grapher_receiver_id();
    reporter_service_id = other.get_reporter_service_id();
    rdma_sender_for_player = other.rdma_sender_for_player;
    rdma_sender_for_grapher = other.rdma_sender_for_grapher;

    other.sender_tl_engine = nullptr;
    other.rdma_sender_for_player = nullptr;
    other.rdma_sender_for_grapher = nullptr;

    return *this;
}


chronolog::DualEndpointChunkExtractorRDMA::~DualEndpointChunkExtractorRDMA()
{
    if(rdma_sender_for_player != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] Destructor: deleting receiver {} ",
                  chl::to_string(player_receiver_service_id));
        delete rdma_sender_for_player;
    }
    if(rdma_sender_for_grapher != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] Destructor: deleting receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
        delete rdma_sender_for_grapher;
    }
}

////

int chronolog::DualEndpointChunkExtractorRDMA::process_chunk(chronolog::StoryChunk* chunk)
{
    if(!rdma_sender_for_grapher)
        return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
    if(rdma_sender_for_player)
        rdma_sender_for_player->transfer_story_chunk(*chunk);
    chl::ChunkReceipt receipt;
    int status = rdma_sender_for_grapher->transfer_story_chunk(*chunk, reporter_service_id, &receipt);
    if(status == chl::CL_SUCCESS)
        chunk->setGrapherReceipt(receipt.grapher_instance, receipt.receipt);
    return status;
}

/*
   JSON Block for Dual Endpoint RDMA Extractor

           "extractor_name": {
                "type": "dual_endpoint_rdma_extractor",
                "player_receiving_endpoint": {
                    "protocol_conf": "ofi+sockets",
                    "service_ip": "127.0.0.1",
                    "service_base_port": 2230,
                    "service_provider_id": 30
                },
                "grapher_receiving_endpoint": {
                    "protocol_conf": "ofi+sockets",
                    "service_ip": "127.0.0.1",
                    "service_base_port": 3333,
                    "service_provider_id": 33
                }

*/
int chronolog::DualEndpointChunkExtractorRDMA::reset(json_object* json_block)
{
    if((json_block == nullptr) || !json_object_is_type(json_block, json_type_object) ||
       (json_object_object_get(json_block, "type") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "type"), json_type_string) ||
       (std::string("dual_endpoint_rdma_extractor")
                .compare(json_object_get_string(json_object_object_get(json_block, "type"))) != 0))
    {
        LOG_ERROR("[DualEndpointExtractorRDMA] Reset failure: invalid json config");
        return chl::CL_ERR_INVALID_CONF;
    }

    if((json_object_object_get(json_block, "grapher_receiving_endpoint") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "grapher_receiving_endpoint"), json_type_object))
    {
        LOG_ERROR("[DualEndpointChunkExtractorRDMA] Reset failure: invalid json config");
        return chl::CL_ERR_INVALID_CONF;
    }

    json_object* json_rpc_block = json_object_object_get(json_block, "grapher_receiving_endpoint");
    chl::RPCProviderConf grapher_receiving_endpoint_conf;
    if(grapher_receiving_endpoint_conf.parseJsonConf(json_rpc_block) != chl::CL_SUCCESS)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Reset failure: invalid grapher_endpoint config");
        return chl::CL_ERR_INVALID_CONF;
    }

    chl::ServiceId grapher_receiver_id(grapher_receiving_endpoint_conf.PROTO_CONF,
                                       grapher_receiving_endpoint_conf.IP,
                                       grapher_receiving_endpoint_conf.BASE_PORT,
                                       grapher_receiving_endpoint_conf.SERVICE_PROVIDER_ID);

    LOG_DEBUG("[ChunkExtractorRDMA] reset: about to create grapher_rdma_sender for receiver_service {} ",
              chl::to_string(grapher_receiver_id));

    if(!grapher_receiver_id.is_valid())
    {
        LOG_ERROR("[DualEndpointChunkExtractorRDMA] Reset failure: invalid grapher endpoint config");
        return chl::CL_ERR_INVALID_CONF;
    }

    restart_rdma_sender_for_grapher(grapher_receiver_id);

    if((json_object_object_get(json_block, "player_receiving_endpoint") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "player_receiving_endpoint"), json_type_object))
    {
        LOG_ERROR("[DualEndpointChunkExtractorRDMA] Reset failure: invalid json config");
        return chl::CL_ERR_INVALID_CONF;
    }

    json_object* player_rpc_block = json_object_object_get(json_block, "player_receiving_endpoint");
    chl::RPCProviderConf player_receiving_endpoint_conf;
    if(player_receiving_endpoint_conf.parseJsonConf(player_rpc_block) != chl::CL_SUCCESS)
    {
        LOG_ERROR("[DualEndpointChunkExtractorRDMA] Reset failure: invalid player endpoint config");
        return chl::CL_ERR_INVALID_CONF;
    }

    chl::ServiceId player_receiver_id(player_receiving_endpoint_conf.PROTO_CONF,
                                      player_receiving_endpoint_conf.IP,
                                      player_receiving_endpoint_conf.BASE_PORT,
                                      player_receiving_endpoint_conf.SERVICE_PROVIDER_ID);

    if(!player_receiver_id.is_valid())
    {
        LOG_ERROR("[ChunkExtractorRDMA] Reset failure: invalid player endpoint config");
        return chl::CL_ERR_INVALID_CONF;
    }

    LOG_DEBUG("[ChunkExtractorRDMA] reset: about to create player_rdma_sender for receiver_service {} ",
              chl::to_string(player_receiver_id));

    restart_rdma_sender_for_player(player_receiver_id);

    return chl::CL_SUCCESS;
}

int chronolog::DualEndpointChunkExtractorRDMA::reset_player_endpoint(chl::ServiceId const& receiver_player)
{

    return chl::CL_SUCCESS;
}

int chronolog::DualEndpointChunkExtractorRDMA::reset_grapher_endpoint(chl::ServiceId const& receiver_grapher)
{

    return chl::CL_SUCCESS;
}
