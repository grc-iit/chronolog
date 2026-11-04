#include <cereal/archives/binary.hpp>
#include <json-c/json.h>
#include <thallium.hpp>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <ConfigurationBlocks.h>
#include <RDMATransferAgent.h>
#include <ChunkExtractorRDMA.h>

namespace tl = thallium;
namespace chl = chronolog;

chronolog::StoryChunkExtractorRDMA::StoryChunkExtractorRDMA(tl::engine* tl_engine,
                                                            chronolog::ServiceId const& receiving_service_id)
    : sender_tl_engine(tl_engine)
    , receiver_service_id(receiving_service_id)
    , rdma_sender(nullptr)
{
    if(receiver_service_id.is_valid())
    {
        try
        {
            rdma_sender = RDMATransferAgent::CreateRDMATransferAgent(*sender_tl_engine, receiver_service_id);
            LOG_TRACE("[ChunkExtractorRDMA] Constructor created rdma_sender for receiver_service {} ",
                      chl::to_string(receiver_service_id));
        }
        catch(...)
        {
            LOG_ERROR("[ChunkExtractorRDMA] Constructor : failed to create rdma_sender for receiver_service {} ",
                      chl::to_string(receiver_service_id));
            rdma_sender = nullptr;
        }
    }
}

chronolog::StoryChunkExtractorRDMA::StoryChunkExtractorRDMA(StoryChunkExtractorRDMA&& other)
{
    if(this == &other)
    {
        return;
    }

    sender_tl_engine = other.get_sender_engine();
    receiver_service_id = other.get_receiver_service_id();
    reporter_service_id = other.get_reporter_service_id();
    rdma_sender = other.rdma_sender;

    LOG_TRACE("[ChunkExtractorRDMA] move constructor: using rdma_sender for receiver_service {} ",
              chl::to_string(receiver_service_id));

    other.sender_tl_engine = nullptr;
    other.rdma_sender = nullptr;
}

chl::StoryChunkExtractorRDMA& chronolog::StoryChunkExtractorRDMA::operator=(StoryChunkExtractorRDMA&& other)
{
    if(this == &other)
    {
        return *this;
    }

    if(rdma_sender != nullptr)
    {
        delete rdma_sender;
    }

    sender_tl_engine = other.get_sender_engine();
    receiver_service_id = other.get_receiver_service_id();
    reporter_service_id = other.get_reporter_service_id();
    rdma_sender = other.rdma_sender;

    LOG_TRACE("[ChunkExtractorRDMA] move operator: using rdma_sender for receiver_service {} ",
              chl::to_string(receiver_service_id));

    other.sender_tl_engine = nullptr;
    other.rdma_sender = nullptr;
    return *this;
}

////

chronolog::StoryChunkExtractorRDMA::~StoryChunkExtractorRDMA()
{
    if(rdma_sender != nullptr)
    {
        LOG_TRACE("[ChunkExtractorRDMA] Destructor: deleting receiver_service {} ",
                  chl::to_string(receiver_service_id));
        delete rdma_sender;
    }
}

//////////////

void chronolog::StoryChunkExtractorRDMA::restart_rdma_sender(chl::ServiceId const& new_receiver_service_id)
{
    LOG_DEBUG("[ChunkExtractorRDMA] restart_rdma_sender for new receiver_service {} ",
              chl::to_string(new_receiver_service_id));

    if(rdma_sender != nullptr)
    {
        LOG_TRACE("[ChunkExtractorRDMA] assingment : deleting receiver_service {} ",
                  chl::to_string(receiver_service_id));

        delete rdma_sender;
    }

    receiver_service_id = new_receiver_service_id;

    if(!receiver_service_id.is_valid())
    {
        return;
    }

    try
    {
        rdma_sender = RDMATransferAgent::CreateRDMATransferAgent(*sender_tl_engine, receiver_service_id);
        LOG_TRACE("[ChunkExtractorRDMA] assingment: created rdma_sender for receiver_service {} ",
                  chl::to_string(receiver_service_id));
    }
    catch(...)
    {
        LOG_ERROR("[ChunkExtractorRDMA] assignment: failed to create rdma_sender for receiver_service {} ",
                  chl::to_string(receiver_service_id));
        rdma_sender = nullptr;
    }
}

////////////

int chronolog::StoryChunkExtractorRDMA::reset(chl::ServiceId const& new_receiver_id)
{
    if(!new_receiver_id.is_valid())
    {
        return chl::CL_ERR_INVALID_CONF;
    }

    LOG_DEBUG("[ChunkExtractorRDMA] reset: about to create rdma_sender for receiver_service {} ",
              chl::to_string(new_receiver_id));

    restart_rdma_sender(new_receiver_id);

    if(is_active())
    {
        return chl::CL_SUCCESS;
    }
    else
    {
        return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
    }
}

////
//   JSON configuration for single endpoint RDMA Chunk Extractor RDMA
//   looks like this:
//
//   "extractor_name": {
//          "type": "single_endpoint_rdma_extractor",
//          "receiving_endpoint": {
//            "protocol_conf": "ofi+sockets",
//            "service_ip": "127.0.0.1",
//            "service_base_port": 2230,
//            "service_provider_id": 30
//          }
//        }

int chronolog::StoryChunkExtractorRDMA::reset(json_object* json_block)
{
    if((json_block == nullptr) || !json_object_is_type(json_block, json_type_object) ||
       (json_object_object_get(json_block, "type") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "type"), json_type_string) ||
       (std::string("single_endpoint_rdma_extractor")
                .compare(json_object_get_string(json_object_object_get(json_block, "type"))) != 0))
    {
        LOG_ERROR("[ChunkExtractorRDMA] Reset failure: invalid json config");
        return chl::CL_ERR_INVALID_CONF;
    }

    if((json_object_object_get(json_block, "receiving_endpoint") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "receiving_endpoint"), json_type_object))
    {
        LOG_ERROR("[ChunkExtractorRDMA] Reset failure: invalid json config");
        return chl::CL_ERR_INVALID_CONF;
    }

    json_object* json_rpc_block = json_object_object_get(json_block, "receiving_endpoint");
    chl::RPCProviderConf receiving_endpoint_conf;
    if(receiving_endpoint_conf.parseJsonConf(json_rpc_block) != chl::CL_SUCCESS)
    {
        LOG_ERROR("[ChunkExtractorRDMA] Reset failure: invalid endpoint config");
        return chl::CL_ERR_INVALID_CONF;
    }

    chl::ServiceId new_receiver_id(receiving_endpoint_conf.PROTO_CONF,
                                   receiving_endpoint_conf.IP,
                                   receiving_endpoint_conf.BASE_PORT,
                                   receiving_endpoint_conf.SERVICE_PROVIDER_ID);

    if(!new_receiver_id.is_valid())
    {
        return chl::CL_ERR_INVALID_CONF;
    }

    LOG_DEBUG("[ChunkExtractorRDMA] reset: about to create rdma_sender for receiver_service {} ",
              chl::to_string(new_receiver_id));

    restart_rdma_sender(new_receiver_id);

    if(is_active())
    {
        return chl::CL_SUCCESS;
    }
    else
    {
        return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
    }
}

////

int chronolog::StoryChunkExtractorRDMA::process_chunk(chronolog::StoryChunk* chunk)
{
    if(!rdma_sender)
        return chl::CL_ERR_STORY_CHUNK_EXTRACTION;
    chl::ChunkReceipt receipt;
    int status = rdma_sender->transfer_story_chunk(*chunk, reporter_service_id, &receipt);
    if(status == chl::CL_SUCCESS)
        chunk->setGrapherReceipt(receipt.grapher_instance, receipt.receipt);
    return status;
}
