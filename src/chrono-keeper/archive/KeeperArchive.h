#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "membership/Watcher.h"
#include "wal/WalJournal.h"

namespace chronolog::keeper
{
struct KeeperArchiveConfig
{
    uint32_t story_chunk_duration_secs{10};
    uint32_t seal_interval_ms{1000};
    size_t chunk_max_bytes{32u << 20};
    size_t frame_bytes{1u << 20};
    uint32_t watermark_resend_timeout_secs{300};
    uint32_t archive_visibility_delay_secs{10};
    uint64_t retention_cap_mb{4096};
};

class KeeperArchive
{
public:
    using Time = std::chrono::steady_clock::time_point;
    using Now = std::function<Time()>;
    KeeperArchive(
            WalJournal& journal,
            const Membership& membership,
            std::string process_id,
            KeeperArchiveConfig config = {},
            Now now = [] { return std::chrono::steady_clock::now(); });
    ~KeeperArchive();
    void start();
    void stop();
    absl::Status seal();
    bool shipOne(std::stop_token stop = {});
    void delivered(const std::string& id, const internal::v1::ChunkReceipt& receipt, size_t bytes);
    void sendFailed(const std::string& id);
    void applyReport(const WatermarkReport& report);
    void releaseTail(StoryId story);
    void sweep();
    std::vector<Chunk> chunks() const;
    Hlc knownWatermark(StoryId story) const;

private:
    struct State
    {
        Chunk chunk;
        size_t bytes{};
        bool delivered{}, settled{}, tail{true}, inflight{};
        std::string instance;
        uint64_t receipt{};
        unsigned failures{};
        Time activity{}, settled_at{}, retry_at{};
    };
    struct ReceiptView
    {
        uint64_t highest{};
        std::set<uint64_t> pending;
    };
    struct StoryState
    {
        Hlc chain_end{}, watermark{};
        bool has_chain{}, dropped{};
        std::map<std::string, ReceiptView> receipts;
    };
    struct Subscription
    {
        std::mutex mu;
        std::set<StoryId> stories;
        std::shared_ptr<grpc::ClientContext> context;
        std::unique_ptr<Watcher> watcher;
    };
    void collectLocked();
    void settleLocked(State& state);
    bool safe(const State& state) const;
    void refreshSubscriptions();
    bool watch(const std::string& endpoint, Subscription& subscription, std::stop_token stop);
    Hlc align(Hlc hlc) const;
    absl::Status addChunk(Chunk chunk, size_t bytes);

    WalJournal& journal_;
    const Membership& membership_;
    const std::string process_id_;
    const KeeperArchiveConfig config_;
    const Now now_;
    mutable std::mutex mu_;
    std::mutex seal_mu_;
    std::map<std::string, State> chunks_;
    std::map<StoryId, StoryState> stories_;
    std::map<std::string, std::unique_ptr<Subscription>> subscriptions_;
    std::condition_variable_any cv_;
    std::jthread sealer_, shipper_;
    bool cap_warned_{};
};
} // namespace chronolog::keeper
