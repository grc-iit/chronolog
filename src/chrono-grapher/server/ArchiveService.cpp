#include "chrono-grapher/server/ArchiveService.h"
#include "chrono-grapher/server/WorkerPool.h"
#include <absl/crc/crc32c.h>
#include <absl/log/log.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <memory>

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

constexpr std::chrono::seconds kDestroyRetryDelay{1};

bool HasPublishedFiles(const FileTierStore& store, StoryId story)
{
    const auto records = store.manifest(story);
    return records.ok() &&
           std::any_of(records->begin(),
                       records->end(),
                       [](const auto& record)
                       { return record.state == ManifestState::Published || record.state == ManifestState::Empty; });
}
} // namespace

ArchiveService::ArchiveService(FileTierStore& store,
                               std::string instance,
                               TransferLimits limits,
                               CompactionSettings compaction,
                               MigrationSettings migration,
                               ScrubSettings scrub)
    : store_(store)
    , instance_(std::move(instance))
    , limits_(limits)
    , compaction_(compaction)
    , pool_(std::make_unique<WorkerPool>(std::max<uint32_t>(1, limits.concurrent_transfers),
                                         std::max<uint32_t>(1, limits.concurrent_transfers)))
    , destroyer_(std::make_unique<WorkerPool>(1, 1))
    , scrub_(scrub)
{
    // Set before the scrub worker starts: it reads the interval for the slow-tier LOST verdict (I13.15) and hands each
    // pass to the tier worker's status file.
    if(!migration.tiers.empty())
    {
        probe_interval_ = std::chrono::milliseconds(migration.probe_interval_ms);
        if(!migration.status_file.empty())
            status_wait_ =
                    std::min<std::chrono::milliseconds>(probe_interval_,
                                                        std::chrono::milliseconds(migration.status_heartbeat_ms));
        else
            status_wait_ = probe_interval_;
        migration_ = std::make_unique<MigrationWorker>(store_, std::move(migration), scrub_.interval.count() > 0);
    }
    if(scrub_.interval.count() > 0)
    {
        scrubber_ = std::make_unique<WorkerPool>(1, 1);
        scrubber_->submit([this] { scrubLoop(); });
    }
    if(migration_)
    {
        migrator_ = std::make_unique<WorkerPool>(1, 1);
        migrator_->submit([this] { migrateLoop(); });
    }
    // A restart resumes the deletion of every tombstoned story that still has Published files, from the manifest
    // alone, without asking the Catalog (I13.11).
    if(const auto stories = store_.tombstonedStories(); stories.ok())
        for(const auto story: *stories)
            if(HasPublishedFiles(store_, story))
                destroy_queue_.push_back(story);
    destroyer_->submit([this] { destroyLoop(); });
    if(compaction_.enabled)
    {
        compactor_ = std::make_unique<WorkerPool>(1, 1);
        compactor_->submit([this] { compactLoop(); });
    }
}

ArchiveService::~ArchiveService() { shutdown(); }

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
                id.watermark_exempt(),
                id.physical_policy()};
    chunk.events.reserve(payload.events_size());
    for(const auto& encoded: payload.events())
    {
        auto event = Decode(encoded);
        if(!event.ok())
            return Fail(*response, grpc::StatusCode::INVALID_ARGUMENT, std::string(event.status().message()));
        chunk.events.push_back(*std::move(event));
    }
    auto publish = [&]() -> grpc::Status
    {
        CHRONOLOG_ASSERT_WORKER_THREAD();
        uint64_t receipt = 0;
        {
            std::lock_guard lock(mutex_);
            // The refusal is decided from the manifest, so it survives a restart. It shares this critical section with
            // the receipt, so a tombstone either refuses the chunk or finds its receipt pending (I13.11).
            const auto tombstoned = store_.tombstoned(chunk.story_id);
            if(!tombstoned.ok())
                return Fail(*response, grpc::StatusCode::UNAVAILABLE, std::string(tombstoned.status().message()));
            if(*tombstoned)
                return Fail(*response, grpc::StatusCode::FAILED_PRECONDITION, "story tombstoned");
            auto& story = receipts_[chunk.story_id];
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
        LOG(INFO) << "archive_published chunk=" << id.chunk_id() << " story=" << id.story_id() << " monotonic_ns="
                  << std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        response->mutable_status()->set_code(0);
        response->set_chunk_id(id.chunk_id());
        response->set_bytes(bytes.size());
        response->set_grapher_instance(instance_);
        response->set_receipt(receipt);
        return grpc::Status::OK;
    };
    std::packaged_task<grpc::Status()> task(std::move(publish));
    auto done = task.get_future();
    if(!pool_->submit([&task] { task(); }))
        return Fail(*response, grpc::StatusCode::RESOURCE_EXHAUSTED, "grapher saturated");
    return done.get();
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
                const auto tombstoned = store_.tombstoned(story_id);
                if(!tombstoned.ok())
                    return {grpc::StatusCode::UNAVAILABLE, std::string(tombstoned.status().message())};
                report.set_dropped(*tombstoned);
                if(state != receipts_.end())
                {
                    report.set_highest_receipt(state->second.highest);
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

void ArchiveService::tombstone(StoryId story)
{
    if(!story)
        return;
    {
        std::lock_guard lock(mutex_);
        if(draining_ || std::find(destroy_queue_.begin(), destroy_queue_.end(), story) != destroy_queue_.end())
            return;
        destroy_queue_.push_back(story);
    }
    changed_.notify_all();
}

std::vector<StoryId> ArchiveService::storiesToConfirm() const
{
    auto stories = store_.liveStories();
    return stories.ok() ? *std::move(stories) : std::vector<StoryId>{};
}

bool ArchiveService::waitDestroyed(StoryId story, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock,
                             timeout,
                             [&]
                             {
                                 const auto recorded = store_.tombstoned(story);
                                 const auto pending = store_.hasPendingUnlinks(story);
                                 return recorded.ok() && *recorded && pending.ok() && !*pending &&
                                        std::find(destroy_queue_.begin(), destroy_queue_.end(), story) ==
                                                destroy_queue_.end();
                             });
}

// One worker (M11.7) takes the queued stories in turn. Per story: the Tombstoned record first, so the dropped report
// and the refusal of late chunks exist before anything is freed; then the wait for the story's pending receipts; then
// the deletion. A failed step requeues the story behind the others and retries after a delay.
void ArchiveService::destroyLoop()
{
    CHRONOLOG_ASSERT_WORKER_THREAD();
    std::unique_lock lock(mutex_);
    auto next_unlink_retry = std::chrono::steady_clock::now() + kDestroyRetryDelay;
    const auto retry_unlinks = [&]
    {
        if(std::chrono::steady_clock::now() < next_unlink_retry)
            return;
        lock.unlock();
        const auto cleanup = store_.retryDeletedFiles();
        if(!cleanup.ok())
            LOG_EVERY_N_SEC(ERROR, 10) << "archive Deleted file cleanup failed: " << cleanup;
        lock.lock();
        next_unlink_retry = std::chrono::steady_clock::now() + kDestroyRetryDelay;
        ++revision_;
        changed_.notify_all();
    };
    size_t requeued = 0;
    while(true)
    {
        if(requeued >= destroy_queue_.size() && requeued)
        {
            changed_.wait_until(lock, next_unlink_retry, [&] { return draining_; });
            requeued = 0;
        }
        changed_.wait_until(lock, next_unlink_retry, [&] { return draining_ || !destroy_queue_.empty(); });
        if(draining_)
            return;
        retry_unlinks();
        if(destroy_queue_.empty())
            continue;
        const auto story = destroy_queue_.front();
        lock.unlock();
        const auto recorded = store_.tombstone(story);
        lock.lock();
        bool done = false;
        if(recorded.ok())
        {
            ++revision_;
            changed_.notify_all();
            const auto ready = [&]
            {
                const auto receipts = receipts_.find(story);
                return draining_ || receipts == receipts_.end() || receipts->second.pending.empty();
            };
            while(!ready())
            {
                changed_.wait_until(lock, next_unlink_retry, ready);
                if(!draining_)
                    retry_unlinks();
            }
            if(draining_)
                return;
            lock.unlock();
            done = eraseFiles(story);
            lock.lock();
        }
        else
        {
            LOG_EVERY_N_SEC(WARNING, 10) << "tombstone record for story " << story << " failed: " << recorded;
        }
        destroy_queue_.pop_front();
        if(done)
        {
            ++revision_;
            changed_.notify_all();
            continue;
        }
        destroy_queue_.push_back(story);
        ++requeued;
        if(destroy_queue_.size() == 1)
            changed_.wait_until(lock, next_unlink_retry, [&] { return draining_ || destroy_queue_.size() > 1; });
    }
}

// A story is finished only after its Published and Empty files and any pending Deleted files have been unlinked.
bool ArchiveService::eraseFiles(StoryId story)
{
    CHRONOLOG_ASSERT_WORKER_THREAD();
    while(true)
    {
        const auto records = store_.manifest(story);
        if(absl::IsNotFound(records.status()))
            return true;
        if(!records.ok())
        {
            LOG_EVERY_N_SEC(WARNING, 10) << "manifest of story " << story << " unreadable: " << records.status();
            return false;
        }
        bool any = false, failed = false;
        for(const auto& record: *records)
        {
            // An Empty window has a file too (I13.3), so it is erased the same way.
            if(record.state != ManifestState::Published && record.state != ManifestState::Empty)
                continue;
            any = true;
            const auto erased = store_.eraseFile(record.file);
            // FAILED_PRECONDITION: compaction superseded the file after this selection; the loop reselects.
            if(!erased.ok() && !absl::IsNotFound(erased) && !absl::IsFailedPrecondition(erased))
            {
                LOG_EVERY_N_SEC(ERROR, 10)
                        << "cannot erase " << record.file << " of tombstoned story " << story << ": " << erased;
                failed = true;
            }
        }
        if(!any)
        {
            (void)store_.retryDeletedFiles();
            const auto pending = store_.hasPendingUnlinks(story);
            return pending.ok() && !*pending;
        }
        if(failed)
            return false;
    }
}

// One job at a time on its own worker (M11.7), never on a transfer worker. Committed cleanup runs before the next
// job. Shutdown stops a job waiting for its budget; a job stopped mid-way leaves only work that recovery resumes.
void ArchiveService::compactLoop()
{
    CHRONOLOG_ASSERT_WORKER_THREAD();
    std::unique_lock lock(mutex_);
    while(!draining_)
    {
        lock.unlock();
        const auto cleanup = store_.retryDeletedFiles();
        if(!cleanup.ok())
            LOG_EVERY_N_SEC(ERROR, 10) << "archive cleanup before compaction failed: " << cleanup;
        // A reserve below what a manifest compaction and a migration pass need could not drain a full `local`.
        if(const auto reserve = store_.hardStopReserve())
            if(const auto bound = store_.hardStopReserveBound(); bound.ok() && reserve < *bound)
                LOG_EVERY_N_SEC(WARNING, 60) << "hard_stop_reserve_bytes " << reserve << " is below the " << *bound
                                             << " bytes the manifest and one migration pass need";
        const auto result = store_.compactOnce(compaction_.policy);
        if(!result.ok() && !absl::IsCancelled(result.status()))
            LOG_EVERY_N_SEC(WARNING, 60) << "archive compaction skipped: " << result.status();
        lock.lock();
        if(result.ok() && result->inputs)
            continue;
        changed_.wait_for(lock, compaction_.scan_interval, [&] { return draining_; });
    }
}

void ArchiveService::migrateLoop()
{
    CHRONOLOG_ASSERT_WORKER_THREAD();
    std::unique_lock lock(mutex_);
    while(!draining_)
    {
        lock.unlock();
        const auto result = migration_->pass();
        if(!result.ok() && !absl::IsCancelled(result))
            LOG_EVERY_N_SEC(WARNING, 60) << "tier worker pass failed: " << result;
        lock.lock();
        changed_.wait_for(lock, status_wait_, [&] { return draining_; });
    }
}

// One pass at a time on its own worker (M11.7). A pass that fails keeps the previous mark, so the next Open
// validates what this pass did not cover.
void ArchiveService::scrubLoop()
{
    CHRONOLOG_ASSERT_WORKER_THREAD();
    std::unique_lock lock(mutex_);
    while(!draining_)
    {
        lock.unlock();
        const auto result = store_.scrubOnce(scrub_.io_bytes_per_sec, scrub_.slow_tiers, probe_interval_);
        if(!result.ok() && !absl::IsCancelled(result.status()))
            LOG_EVERY_N_SEC(WARNING, 60) << "archive scrub pass failed: " << result.status();
        else if(result.ok())
            LOG(INFO) << "archive scrub validated=" << result->validated << " skipped=" << result->skipped
                      << " lost=" << result->lost << " rolled_back=" << result->rolled_back
                      << " slow_failed=" << result->slow_failed << " through=" << result->through;
        if(migration_ && !absl::IsCancelled(result.status()))
            migration_->scrubbed(result,
                                 std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
        lock.lock();
        changed_.wait_for(lock, scrub_.interval, [&] { return draining_; });
    }
}

void ArchiveService::shutdown()
{
    {
        std::lock_guard lock(mutex_);
        draining_ = true;
    }
    changed_.notify_all();
    if(compactor_)
        store_.stopCompaction();
    // Joined here so no deletion or compaction is still touching the store when the caller releases it.
    if(migration_)
        migration_->stop();
    if(scrubber_)
        store_.stopScrub();
    destroyer_->stop();
    if(migrator_)
        migrator_->stop();
    if(scrubber_)
        scrubber_->stop();
    if(compactor_)
        compactor_->stop();
}
} // namespace chronolog::grapher
