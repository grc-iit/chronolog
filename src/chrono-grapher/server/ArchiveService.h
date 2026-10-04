#pragma once

#include "tier/FileTierStore.h"
#include "chrono-grapher/server/MigrationWorker.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <set>

namespace chronolog::grapher
{
class WorkerPool;
struct TransferLimits
{
    uint64_t chunk_bytes = 64 * 1024 * 1024;
    uint64_t frame_bytes = 1024 * 1024;
    uint32_t concurrent_transfers = 8;
};

// Background compaction of small archive files (B3). Disabled by default: it may be enabled only once every Player
// and Grapher reading the archive root understands compact_v1 lines.
struct CompactionSettings
{
    bool enabled = false;
    std::chrono::seconds scan_interval{60};
    CompactionPolicy policy;
};

// The scrubber of I13.17: one pass validates every own effective file on `local` and replaces the validated mark that
// bounds the next Open. interval zero disables it, and with it migration, which moves only files a mark covers.
struct ScrubSettings
{
    std::chrono::seconds interval{0};
    uint64_t io_bytes_per_sec = 4 * 1024 * 1024;
    bool slow_tiers = false;
};

class ArchiveService final: public internal::v1::Archive::Service
{
public:
    ArchiveService(FileTierStore& store,
                   std::string instance,
                   TransferLimits limits = {},
                   CompactionSettings compaction = {},
                   MigrationSettings migration = {},
                   ScrubSettings scrub = {});
    ~ArchiveService() override;
    grpc::Status TransferChunk(grpc::ServerContext*,
                               grpc::ServerReader<internal::v1::TransferChunkRequest>*,
                               internal::v1::TransferChunkResponse*) override;
    grpc::Status WatchWatermarks(grpc::ServerContext*,
                                 const internal::v1::WatchWatermarksRequest*,
                                 grpc::ServerWriter<internal::v1::WatchWatermarksResponse>*) override;
    // Queues the destruction of a story the Catalog reports tombstoned (RFC-C, I13.11). A worker appends the
    // Tombstoned manifest record, waits until no receipt of the story is pending, then erases every Published file
    // through Deleted records. Idempotent, and a failed step is retried.
    void tombstone(StoryId story);
    // Stories whose data this Grapher holds that no tombstone covers; the Catalog confirms each after a snapshot.
    std::vector<StoryId> storiesToConfirm() const;
    // True once the story's Tombstoned record is durable and no deletion is queued or running for it.
    bool waitDestroyed(StoryId story, std::chrono::milliseconds timeout);
    void shutdown();

private:
    void destroyLoop();
    void compactLoop();
    void migrateLoop();
    void scrubLoop();
    bool eraseFiles(StoryId story);
    struct Receipts
    {
        uint64_t highest{};
        std::set<uint64_t> pending;
    };
    FileTierStore& store_;
    const std::string instance_;
    const TransferLimits limits_;
    const CompactionSettings compaction_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<StoryId, Receipts> receipts_;
    uint64_t next_receipt_{};
    uint32_t active_{};
    uint64_t revision_{};
    bool draining_{};
    std::deque<StoryId> destroy_queue_;
    std::unique_ptr<WorkerPool> pool_;
    std::unique_ptr<WorkerPool> destroyer_;
    std::unique_ptr<WorkerPool> compactor_;
    std::unique_ptr<WorkerPool> migrator_;
    std::unique_ptr<WorkerPool> scrubber_;
    const ScrubSettings scrub_;
    std::unique_ptr<MigrationWorker> migration_;
    std::chrono::milliseconds probe_interval_{};
};
} // namespace chronolog::grapher
