#ifndef STORY_CHUNK_CONSUMER_SERVICE_H
#define STORY_CHUNK_CONSUMER_SERVICE_H
#include <thallium.hpp>
#include <ChunkReceipt.h>
#include <ServiceId.h>
#include <private/RpcBulkOwner.h>
#include <private/RpcBulkControl.h>
#include <private/RpcBulkSerialization.h>
#include <private/Crc32c.h>
#include "StoryChunkIngestionQueue.h"
namespace tl = thallium;
namespace chronolog
{
class StoryChunkConsumerService: public tl::provider<StoryChunkConsumerService>
{
public:
    static StoryChunkConsumerService*
    CreateChunkConsumerService(tl::engine& engine, uint16_t provider, StoryChunkIngestionQueue& queue)
    {
        return CreateChunkConsumerService(rpc::RpcBulkOwner::Find(engine.get_margo_instance()), provider, queue);
    }
    static StoryChunkConsumerService* CreateChunkConsumerService(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                                                                 uint16_t provider,
                                                                 StoryChunkIngestionQueue& queue)
    {
        if(!owner || owner->closing)
            return nullptr;
        if(owner->provider)
            throw std::logic_error("Chunk consumer provider already exists");
        auto service =
                std::shared_ptr<StoryChunkConsumerService>(new StoryChunkConsumerService(owner, provider, queue));
        owner->provider = service;
        service->define("receiver_available", &StoryChunkConsumerService::receiver_available);
        service->define("receive_story_chunk", &StoryChunkConsumerService::receive_story_chunk);
        return service.get();
    }
    ~StoryChunkConsumerService() = default;
    void receiver_available(tl::request const& request) noexcept { rpc::bulkReady(owner_.lock(), request); }
    void receive_story_chunk(tl::request const& request, tl::bulk& source, ServiceId const&, uint32_t checksum) noexcept
    {
        auto owner = owner_.lock();
        if(!owner || owner->closing)
            return;
        auto lease = owner->incoming.acquire();
        if(!lease)
        {
            // No free receive slot: say so instead of leaving the keeper to
            // wait for an answer that never comes. Nothing was pulled.
            ChunkReceipt busy;
            busy.status = CL_ERR_RPC_CAPACITY;
            owner->incoming.refuse(request, busy);
            return;
        }
        struct Receive: rpc::BulkReceiveContext
        {
            using rpc::BulkReceiveContext::BulkReceiveContext;
            std::unique_ptr<StoryChunk> chunk;
        };
        std::shared_ptr<Receive> context;
        try
        {
            context = std::make_shared<Receive>(owner,
                                                *owner->engine,
                                                request,
                                                source,
                                                owner->incoming.observers,
                                                owner->incoming.pieceBytes);
            lease->retain(context);
            if(!context->pull())
            {
                lease->quarantine();
                return;
            }
            context->chunk = std::make_unique<StoryChunk>();
            if(rpc::crc32c(context->bytes.data(), context->bytes.size()) != checksum ||
               !rpc::deserializeBulk(context->bytes.data(), context->bytes.size(), *context->chunk))
            {
                if(!context->answer(ChunkReceipt{}))
                    lease->quarantine();
                return;
            }
            queue_.ingestStoryChunk(context->chunk.get());
            context->chunk.release();
            ChunkReceipt reply;
            reply.bytes = context->bytes.size();
            if(!context->answer(reply))
                lease->quarantine();
        }
        catch(...)
        {
            if(context)
            {
                if((context->initiated && !context->completed) || context->chunk)
                {
                    lease->quarantine();
                    return;
                }
                if(!context->answer(ChunkReceipt{}))
                    lease->quarantine();
            }
        }
    }

private:
    StoryChunkConsumerService(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                              uint16_t provider,
                              StoryChunkIngestionQueue& queue)
        : tl::provider<StoryChunkConsumerService>(*owner->engine, provider)
        , owner_(owner)
        , queue_(queue)
    {}
    std::weak_ptr<rpc::RpcBulkOwner> owner_;
    StoryChunkIngestionQueue& queue_;
};
} // namespace chronolog
#endif
