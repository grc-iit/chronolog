#ifndef GRAPHER_RECORDING_SERVICE_H
#define GRAPHER_RECORDING_SERVICE_H
#include <functional>
#include <string_view>
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
        StoryId story = 0;
        StoryWatermarkRegistry::Delivery delivery;
        bool fresh = false;
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
                // A keeper that got no answer sends the same chunk again. An
                // identical delivery whose events are already written gets
                // the first one's receipt and is not ingested a second time.
                story = chunk->getStoryId();
                delivery.reporter = to_string(reporter);
                delivery.start = chunk->getStartTime();
                delivery.end = chunk->getEndTime();
                delivery.size = context->bytes.size();
                delivery.checksum = checksum;
                delivery.hash =
                        std::hash<std::string_view>{}(std::string_view(context->bytes.data(), context->bytes.size()));
                reply.grapher_instance = registry_->instanceId();
                reply.receipt = registry_->receiptForDelivery(story, delivery, fresh, &incarnation);
                if(!fresh)
                {
                    LOG_INFO("[GrapherRecordingService] StoryId={} chunk {}-{} from {} is already written; "
                             "answering its receipt {}",
                             story,
                             delivery.start,
                             delivery.end,
                             delivery.reporter,
                             reply.receipt);
                    if(!context->answer(reply))
                        lease->quarantine();
                    return;
                }
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
                    // Not ingested: the keeper's next send must not be taken
                    // for a duplicate.
                    if(fresh && context->chunk && registry_)
                        registry_->forgetDelivery(story, delivery);
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
