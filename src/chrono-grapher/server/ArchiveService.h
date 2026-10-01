#pragma once

#include "chrono-grapher/tier/FileTierStore.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include <condition_variable>
#include <set>

namespace chronolog::grapher
{
struct TransferLimits
{
    uint64_t chunk_bytes = 64 * 1024 * 1024;
    uint64_t frame_bytes = 1024 * 1024;
    uint32_t concurrent_transfers = 8;
};

class ArchiveService final: public internal::v1::Archive::Service
{
public:
    ArchiveService(FileTierStore& store, std::string instance, TransferLimits limits = {});
    grpc::Status TransferChunk(grpc::ServerContext*,
                               grpc::ServerReader<internal::v1::TransferChunkRequest>*,
                               internal::v1::TransferChunkResponse*) override;
    grpc::Status WatchWatermarks(grpc::ServerContext*,
                                 const internal::v1::WatchWatermarksRequest*,
                                 grpc::ServerWriter<internal::v1::WatchWatermarksResponse>*) override;
    void dropStory(StoryId story);
    void shutdown();

private:
    struct Receipts
    {
        uint64_t highest{};
        std::set<uint64_t> pending;
        bool dropped{};
    };
    FileTierStore& store_;
    const std::string instance_;
    const TransferLimits limits_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<StoryId, Receipts> receipts_;
    uint64_t next_receipt_{};
    uint32_t active_{};
    uint64_t revision_{};
    bool draining_{};
};
} // namespace chronolog::grapher
