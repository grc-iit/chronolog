#include "chrono-grapher/server/ArchiveService.h"
#include <absl/crc/crc32c.h>
#include <chrono>
#include <limits>
#include <iostream>
#include <syncstream>

namespace chronolog::grapher
{
namespace
{
namespace wire = internal::v1;

grpc::Status Fail(wire::TransferChunkResponse& response, grpc::StatusCode code, const std::string& message)
{
    response.Clear();
    response.mutable_status()->set_code(static_cast<int>(code));
    response.mutable_status()->set_message(message);
    return {code, message};
}

absl::StatusOr<Event> Decode(const v1::Event& input)
{
    if(!input.has_id() || !input.has_hlc() || !v1::Durability_IsValid(input.durability()) ||
       !v1::ClockStatus_IsValid(input.physical().status()))
        return absl::InvalidArgumentError("invalid Event");
    Event event;
    event.id = {input.id().story_id(), input.id().writer_id(), input.id().incarnation(), input.id().sequence()};
    event.hlc = {input.hlc().physical_ns(), input.hlc().logical()};
    event.physical.physical_ns = input.physical().physical_ns();
    if(input.physical().has_uncertainty_ns())
        event.physical.uncertainty_ns = input.physical().uncertainty_ns();
    switch(input.physical().status())
    {
        case v1::CLOCK_STATUS_SYNCED:
            event.physical.status = ClockStatus::Synced;
            break;
        case v1::CLOCK_STATUS_UNSYNCED:
            event.physical.status = ClockStatus::Unsynced;
            break;
        default:
            event.physical.status = ClockStatus::Unavailable;
            break;
    }
    event.durability = static_cast<Durability>(input.durability());
    event.envelope.content_type = input.envelope().content_type();
    event.envelope.payload = input.envelope().payload();
    event.envelope.trace_id = input.envelope().trace_id();
    event.envelope.span_id = input.envelope().span_id();
    for(const auto& [key, value]: input.envelope().attributes()) event.envelope.attributes[key] = value;
    return event;
}

uint32_t Checksum(const std::string& bytes)
{
    uint32_t result = 0;
    for(unsigned char byte: bytes) result = (result << 8) | byte;
    return result;
}
} // namespace

ArchiveService::ArchiveService(FileTierStore& store, std::string instance, TransferLimits limits)
    : store_(store)
    , instance_(std::move(instance))
    , limits_(limits)
{}

grpc::Status ArchiveService::TransferChunk(grpc::ServerContext* context,
                                           grpc::ServerReader<wire::TransferChunkRequest>* reader,
                                           wire::TransferChunkResponse* response)
{
    {
        std::lock_guard lock(mutex_);
        if(draining_)
            return Fail(*response, grpc::StatusCode::UNAVAILABLE, "grapher draining");
        if(active_ >= limits_.concurrent_transfers)
            return Fail(*response, grpc::StatusCode::RESOURCE_EXHAUSTED, "too many transfers");
        ++active_;
    }
    struct Release
    {
        ArchiveService& service;
        ~Release()
        {
            std::lock_guard lock(service.mutex_);
            --service.active_;
        }
    } release{*this};
    wire::TransferChunkRequest frame, first;
    std::string bytes;
    bool seen = false;
    bool final = false;
    while(reader->Read(&frame))
    {
        if(final)
            return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, "frame after final");
        if(!seen)
        {
            const auto& id = frame.identity();
            const Hlc start{id.start().physical_ns(), id.start().logical()};
            const Hlc end{id.end().physical_ns(), id.end().logical()};
            if(!frame.has_identity() || !id.story_id() || id.chunk_id().empty() || id.chunk_id().size() > 128 ||
               !id.has_start() || !id.has_end() || start >= end || frame.total_bytes() > limits_.chunk_bytes ||
               frame.total_bytes() > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
               frame.checksum().size() != 4 ||
               (frame.checksum_algorithm() != wire::CHECKSUM_ALGORITHM_UNSPECIFIED &&
                frame.checksum_algorithm() != wire::CHECKSUM_ALGORITHM_CRC32C))
                return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, "invalid chunk header");
            first = frame;
            first.clear_data();
            seen = true;
        }
        if(frame.identity().SerializeAsString() != first.identity().SerializeAsString() ||
           frame.total_bytes() != first.total_bytes() || frame.checksum() != first.checksum() ||
           frame.checksum_algorithm() != first.checksum_algorithm() || frame.offset() != bytes.size() ||
           frame.data().size() > limits_.frame_bytes || frame.data().size() > first.total_bytes() - bytes.size() ||
           (frame.data().empty() && !frame.final()))
            return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, "inconsistent or gapped chunk frame");
        bytes += frame.data();
        final = frame.final();
        if(final != (bytes.size() == first.total_bytes()))
            return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, "final frame byte count mismatch");
    }
    if(context->IsCancelled())
        return Fail(*response, grpc::StatusCode::CANCELLED, "transfer cancelled");
    if(!seen || !final)
        return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, "partial chunk stream");
    if(static_cast<uint32_t>(absl::ComputeCrc32c(bytes)) != Checksum(first.checksum()))
        return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, "chunk checksum mismatch");
    wire::ChunkPayload payload;
    if(!payload.ParseFromString(bytes) || payload.events_size() > 65536)
        return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, "invalid ChunkPayload");
    const auto& id = first.identity();
    Chunk chunk{id.chunk_id(),
                id.story_id(),
                {id.start().physical_ns(), id.start().logical()},
                {id.end().physical_ns(), id.end().logical()},
                {},
                id.watermark_exempt()};
    chunk.events.reserve(payload.events_size());
    for(const auto& encoded: payload.events())
    {
        auto event = Decode(encoded);
        if(!event.ok())
            return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, std::string(event.status().message()));
        chunk.events.push_back(*std::move(event));
    }
    uint64_t receipt = 0;
    {
        std::lock_guard lock(mutex_);
        auto& story = receipts_[chunk.story_id];
        if(story.dropped)
            return Fail(*response, grpc::StatusCode::NOT_FOUND, "story dropped");
        if(next_receipt_ == std::numeric_limits<uint64_t>::max())
            return Fail(*response, grpc::StatusCode::RESOURCE_EXHAUSTED, "receipt counter exhausted");
        auto known = store_.contiguousWatermark(chunk.story_id);
        if(!known.ok())
        {
            if(!absl::IsNotFound(known.status()))
                return Fail(*response, grpc::StatusCode::UNAVAILABLE, std::string(known.status().message()));
            const auto registered = store_.registerStory(chunk.story_id);
            if(!registered.ok())
                return Fail(*response, grpc::StatusCode::UNAVAILABLE, std::string(registered.message()));
        }
        receipt = ++next_receipt_;
        story.highest = receipt;
        story.pending.insert(receipt);
        ++revision_;
        changed_.notify_all();
    }
    auto published = store_.publish(std::move(chunk));
    {
        std::lock_guard lock(mutex_);
        receipts_[id.story_id()].pending.erase(receipt);
        ++revision_;
        changed_.notify_all();
    }
    if(!published.ok())
        return Fail(*response,
                    static_cast<grpc::StatusCode>(published.status().code()),
                    std::string(published.status().message()));
    if(published->state != ManifestState::Published && published->state != ManifestState::Empty)
        return Fail(*response, grpc::StatusCode::UNAVAILABLE, "chunk was not persisted");
    std::osyncstream(std::clog) << "archive_published chunk=" << id.chunk_id() << " story=" << id.story_id()
                                << " monotonic_ns="
                                << std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count()
                                << std::endl;
    response->mutable_status()->set_code(0);
    response->set_chunk_id(id.chunk_id());
    response->set_bytes(bytes.size());
    response->set_grapher_instance(instance_);
    response->set_receipt(receipt);
    return grpc::Status::OK;
}

grpc::Status ArchiveService::WatchWatermarks(grpc::ServerContext* context,
                                             const wire::WatchWatermarksRequest* request,
                                             grpc::ServerWriter<wire::WatchWatermarksResponse>* writer)
{
    if(request->keeper_id().empty() || request->story_ids().empty() || request->story_ids_size() > 65536)
        return {grpc::StatusCode::INVALID_ARGUMENT, "keeper and bounded story subscription required"};
    std::set<StoryId> stories(request->story_ids().begin(), request->story_ids().end());
    if(stories.contains(0))
        return {grpc::StatusCode::INVALID_ARGUMENT, "zero story id"};
    std::map<StoryId, std::string> previous;
    while(!context->IsCancelled())
    {
        uint64_t observed;
        std::vector<wire::WatchWatermarksResponse> reports;
        {
            std::lock_guard lock(mutex_);
            if(draining_)
                return grpc::Status::OK;
            observed = revision_;
            for(const auto story_id: stories)
            {
                const auto state = receipts_.find(story_id);
                wire::WatchWatermarksResponse report;
                report.set_story_id(story_id);
                report.set_grapher_instance(instance_);
                auto w = store_.contiguousWatermark(story_id);
                if(w.ok())
                {
                    report.mutable_watermark()->set_physical_ns(w->physical_ns);
                    report.mutable_watermark()->set_logical(w->logical);
                }
                else if(!absl::IsNotFound(w.status()))
                    return {grpc::StatusCode::UNAVAILABLE, std::string(w.status().message())};
                else
                    report.mutable_watermark();
                if(state != receipts_.end())
                {
                    report.set_highest_receipt(state->second.highest);
                    report.set_dropped(state->second.dropped);
                    for(const auto pending: state->second.pending) report.add_pending_receipts(pending);
                }
                const auto encoded = report.SerializeAsString();
                if(!previous.contains(story_id) || previous[story_id] != encoded)
                {
                    previous[story_id] = encoded;
                    reports.push_back(std::move(report));
                }
            }
        }
        for(const auto& report: reports)
            if(!writer->Write(report))
                return {grpc::StatusCode::CANCELLED, "watch closed"};
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(100), [&] { return draining_ || revision_ != observed; });
    }
    return {grpc::StatusCode::CANCELLED, "watch cancelled"};
}

void ArchiveService::dropStory(StoryId story)
{
    std::lock_guard lock(mutex_);
    receipts_[story].dropped = true;
    ++revision_;
    changed_.notify_all();
}

void ArchiveService::shutdown()
{
    std::lock_guard lock(mutex_);
    draining_ = true;
    changed_.notify_all();
}
} // namespace chronolog::grapher
