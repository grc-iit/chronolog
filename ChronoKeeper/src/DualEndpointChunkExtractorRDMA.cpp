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
        tl::engine& tl_engine,
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
                RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, grapher_receiver_service_id);
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
                RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, player_receiver_service_id);
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


chronolog::DualEndpointChunkExtractorRDMA::DualEndpointChunkExtractorRDMA(DualEndpointChunkExtractorRDMA const& other)
    : sender_tl_engine(other.get_sender_engine())
    , player_receiver_service_id(other.get_player_receiver_id())
    , grapher_receiver_service_id(other.get_grapher_receiver_id())
    , rdma_sender_for_player(nullptr)
    , rdma_sender_for_grapher(nullptr)
{
    try
    {
        rdma_sender_for_player =
                RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, player_receiver_service_id);
        LOG_TRACE("[DualEndpointExtractorRDMA] Copy Constructor created rdma_sender for player_receiver {} ",
                  chl::to_string(player_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointExtractorRDMA] Constructor : failed to create rdma_sender for player_receiver {} ",
                  chl::to_string(player_receiver_service_id));
        rdma_sender_for_player = nullptr;
    }

    try
    {
        rdma_sender_for_grapher =
                RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, grapher_receiver_service_id);
        LOG_TRACE("Dual[DualEndpointExtractorRDMA] Copy Constructor created rdma_sender for grpaher_receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointExtractorRDMA] Constructor : failed to create rdma_sender for grapher_receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
        rdma_sender_for_grapher = nullptr;
    }
}

chl::DualEndpointChunkExtractorRDMA&
chronolog::DualEndpointChunkExtractorRDMA::operator=(DualEndpointChunkExtractorRDMA const& other)
{
    if(this == &other)
    {
        return *this;
    }

    if(rdma_sender_for_player != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] assingment : deleting receiver {} ",
                  chl::to_string(player_receiver_service_id));
        delete rdma_sender_for_player;
    }
    if(rdma_sender_for_grapher != nullptr)
    {
        LOG_TRACE("[DualEndpointExtractorRDMA] assingment : deleting receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
        delete rdma_sender_for_grapher;
    }

    sender_tl_engine = other.get_sender_engine();
    player_receiver_service_id = other.get_player_receiver_id();
    grapher_receiver_service_id = other.get_grapher_receiver_id();

    try
    {
        rdma_sender_for_player =
                RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, player_receiver_service_id);
        LOG_TRACE("[DualEndpointExtractorRDMA] assingment: created rdma_sender for receiver {} ",
                  chl::to_string(player_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointExtractorRDMA] assignment: failed to create rdma_sender for receiver {} ",
                  chl::to_string(player_receiver_service_id));
        rdma_sender_for_player = nullptr;
    }

    try
    {
        rdma_sender_for_grapher =
                RDMATransferAgent::CreateRDMATransferAgent(sender_tl_engine, grapher_receiver_service_id);
        LOG_TRACE("[DualEndpointExtractorRDMA] assingment: created rdma_sender for receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointExtractorRDMA] assignment: failed to create rdma_sender for receiver {} ",
                  chl::to_string(grapher_receiver_service_id));
        rdma_sender_for_grapher = nullptr;
    }

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

int chronolog::DualEndpointChunkExtractorRDMA::process_chunk(chronolog::StoryChunk* story_chunk)
{

    // Note: a failure to transfer a story_chunk to the ChronoPlayer alone is not considered a critical enough error
    // to exit this function and try again later. In this case the error would be logged but the function would
    // proceed to sending the story_chunk to the ChronoGrapher
    // Failure to transfer the story_chunk to the ChronoGrapher would trigger the exit from the function,
    // return the StoryChunk back on the extraction_queue, and subsequent retry

    int transfer_return = chl::CL_ERR_UNKNOWN;

    try
    {
        LOG_DEBUG("[DualEndpointChunkExtractor] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",
                  thallium::thread::self_id(),
                  story_chunk->getStoryId(),
                  story_chunk->getChronicleName(),
                  story_chunk->getStoryName(),
                  story_chunk->getStartTime(),
                  story_chunk->getEndTime(),
                  story_chunk->getEventCount());

        if(rdma_sender_for_grapher == nullptr)
        {
            LOG_ERROR("[DualEndpointChunkExtractor] Failed to transfer StoryChunk StoryId={} StartTime={}, no valid "
                      "rdma_sender_for_grapher",
                      story_chunk->getStoryId(),
                      story_chunk->getStartTime());
            return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
        }

        std::ostringstream oss(std::ios::binary);
        try
        {
            cereal::BinaryOutputArchive oarchive(oss);
            oarchive(*story_chunk);
        }
        catch(cereal::Exception const& ex)
        {
            LOG_ERROR("[DualEndpointChunkExtractor] Cereal exception while serializing StoryChunk StoryId={} "
                      "StartTime={} ex {}",
                      story_chunk->getStoryId(),
                      story_chunk->getStartTime(),
                      ex.what());
        }


        std::string serialized_story_chunk = oss.str();

        if(rdma_sender_for_player != nullptr)
        {
            try
            {
                transfer_return = rdma_sender_for_player->transfer_serialized_story_chunk(serialized_story_chunk);
                if(transfer_return == chl::CL_SUCCESS)
                {
                    LOG_INFO("[DualEndpointChunkExtractor] Transfered to Player StoryChunk StoryId={} StartTime={}",
                             story_chunk->getStoryId(),
                             story_chunk->getStartTime());
                }
                else
                {
                    LOG_ERROR("[DualEndpointChunkExtractor] Failed to transfer to Player StoryChunk StoryId={} "
                              "StartTime={}",
                              story_chunk->getStoryId(),
                              story_chunk->getStartTime());
                }
            }
            catch(std::exception const& ex)
            {
                LOG_ERROR("[DualEndpointChunkExtractor] Standard exception while serializing StoryChunk StoryId={} "
                          "StartTime={} ex {}",
                          story_chunk->getStoryId(),
                          story_chunk->getStartTime(),
                          ex.what());
            }
        }

        transfer_return = rdma_sender_for_grapher->transfer_serialized_story_chunk(serialized_story_chunk);

        if(transfer_return == chl::CL_SUCCESS)
        {
            LOG_INFO("[DualEndpointChunkExtractor] Transfered to Grapher StoryChunk StoryId={} StartTime={}",
                     story_chunk->getStoryId(),
                     story_chunk->getStartTime());
        }
        else
        {
            LOG_ERROR("[DualEndpointChunkExtractor] Failed to transfer to Grapher StoryChunk StoryId={} StartTime={}",
                      story_chunk->getStoryId(),
                      story_chunk->getStartTime());
        }

        return transfer_return;
    }
    catch(std::exception const& ex)
    {
        LOG_ERROR("[DualEndpointChunkExtractor] Standard exception while serializing StoryChunk StoryId={} "
                  "StartTime={} ex {}",
                  story_chunk->getStoryId(),
                  story_chunk->getStartTime(),
                  ex.what());
    }
    catch(...)
    {
        LOG_ERROR("[DualEndpointChunkExtractor] Exception while transferring StoryChunkiStoryId={} StartTime={}",
                  story_chunk->getStoryId(),
                  story_chunk->getStartTime());
    }

    return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
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
