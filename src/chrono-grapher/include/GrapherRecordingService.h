#ifndef GRAPHER_RECORDING_SERVICE_H
#define GRAPHER_RECORDING_SERVICE_H
#include <thallium.hpp>
#include <StoryChunk.h>
#include <ChunkReceipt.h>
#include <ServiceId.h>
#include <private/RpcBulkOwner.h>
#include <private/RpcBulkSerialization.h>
#include <private/Crc32c.h>
#include <chrono_monitor.h>
#include "ChunkIngestionQueue.h"
#include "StoryWatermarkRegistry.h"
#include "WatermarkReportPublisher.h"
namespace tl = thallium;
namespace chronolog
{
class GrapherRecordingService: public tl::provider<GrapherRecordingService>
{
public:
    static GrapherRecordingService* CreateRecordingService(tl::engine& engine,
                                                           uint16_t provider,
                                                           ChunkIngestionQueue& queue,
                                                           std::shared_ptr<WatermarkReportPublisher> publisher = {},
                                                           StoryWatermarkRegistry* registry = nullptr)
    {
        return CreateRecordingService(rpc::RpcBulkOwner::Find(engine.get_margo_instance()),
                                      provider,
                                      queue,
                                      publisher,
                                      registry);
    }
    static GrapherRecordingService* CreateRecordingService(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                                                           uint16_t provider,
                                                           ChunkIngestionQueue& queue,
                                                           std::shared_ptr<WatermarkReportPublisher> publisher = {},
                                                           StoryWatermarkRegistry* registry = nullptr)
    {
        if(!owner || owner->closing)
            return nullptr;
        if(owner->provider)
            throw std::logic_error("Grapher recording provider already exists");
        auto service = std::shared_ptr<GrapherRecordingService>(
                new GrapherRecordingService(owner, provider, queue, publisher, registry));
        owner->provider = service; // Root actual provider BEFORE the first callback.
        service->define("receive_story_chunk", &GrapherRecordingService::receive_story_chunk);
        return service.get();
    }
    ~GrapherRecordingService() = default;
    void receive_story_chunk(tl::request const& request,
                             tl::bulk& source,
                             ServiceId const& reporter,
                             uint32_t checksum) noexcept
    {
        auto owner = owner_.lock();
        if(!owner || owner->closing)
            return;
        auto lease = owner->incoming.acquire();
        if(!lease)
            return; // No uncharged potentially blocking rejection send.
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
            // A chunk whose bytes differ from what the keeper sent is refused
            // with nothing ingested; the keeper keeps it and sends it again.
            context->chunk = std::make_unique<StoryChunk>();
            if(rpc::crc32c(context->bytes.data(), context->bytes.size()) != checksum ||
               !rpc::deserializeBulk(context->bytes.data(), context->bytes.size(), *context->chunk))
            {
                LOG_WARNING("[GrapherRecordingService] refused a chunk of {} bytes from {}: checksum or format "
                            "does not match",
                            context->bytes.size(),
                            to_string(reporter));
                if(!context->answer(ChunkReceipt{}))
                    lease->quarantine();
                return;
            }
            auto* chunk = context->chunk.get();
            StoryWatermarkRegistry::Incarnation incarnation;
            ChunkReceipt reply;
            reply.bytes = context->bytes.size();
            if(registry_)
            {
                reply.grapher_instance = registry_->instanceId();
                reply.receipt = registry_->assignReceipt(chunk->getStoryId(), &incarnation);
                chunk->carryReceipt(reply.receipt);
            }
            if(publisher_)
                publisher_->recordContributor(chunk->getStoryId(), reporter, std::move(incarnation));
            queue_.ingestStoryChunk(chunk);
            context->chunk.release(); // Received data survives a failed reply.
            if(!context->answer(reply))
                lease->quarantine();
        }
        catch(...)
        {
            if(context)
            {
                // Keep a received chunk when ingestion or native work is
                // uncertain. Never let the failure-response throw outwards.
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
    GrapherRecordingService(std::shared_ptr<rpc::RpcBulkOwner> const& owner,
                            uint16_t provider,
                            ChunkIngestionQueue& queue,
                            std::shared_ptr<WatermarkReportPublisher> publisher,
                            StoryWatermarkRegistry* registry)
        : tl::provider<GrapherRecordingService>(*owner->engine, provider)
        , owner_(owner)
        , queue_(queue)
        , publisher_(publisher)
        , registry_(registry)
    {}
    std::weak_ptr<rpc::RpcBulkOwner> owner_;
    ChunkIngestionQueue& queue_;
    std::shared_ptr<WatermarkReportPublisher> publisher_;
    StoryWatermarkRegistry* registry_;
};
} // namespace chronolog
#endif
