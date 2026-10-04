#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <set>
#include <thread>

#include "journal/RamJournal.h"
#include "wal/FileSink.h"

namespace chronolog
{

inline constexpr uint32_t kMaxGroupCommitWindowUs = 10'000;

struct WalJournalConfig
{
    std::string wal_dir{"wal"};
    size_t group_commit_max_bytes{4u << 20};
    uint32_t reserve_ahead_ms{1000};
    uint64_t wal_max_bytes{1ull << 30};
    uint64_t wal_segment_bytes{64ull << 20};
    // Zero commits whatever is queued as soon as the committer is free; nonzero holds a group open
    // until this long after its first record or until group_commit_max_bytes is queued (I5.10).
    uint32_t group_commit_window_us{0};
    // I13.16: new appends are refused CAPACITY while the WAL file system has fewer free bytes than this, so the WAL
    // never fails for space. The committer samples it; zero disables the reserve.
    uint64_t wal_reserve_bytes{256ull << 20};
    // Free bytes available to the Keeper on the file system holding a directory; statvfs when unset.
    std::function<absl::StatusOr<uint64_t>(const std::string& dir)> free_bytes;
};

class WalJournal: public RamJournal
{
public:
    using SinkFactory = std::function<std::unique_ptr<FileSink>(const std::string&)>;
    WalJournal(std::shared_ptr<Clock> clock,
               std::shared_ptr<const Membership> membership,
               RamJournalConfig ram_config = {},
               WalJournalConfig config = {},
               SinkFactory sink_factory = openFileSink);
    ~WalJournal() override;
    struct SealedChunk
    {
        Chunk chunk;
        bool settled{};
    };
    std::vector<SealedChunk> sealedChunks() const;
    const std::string& recoveredInstance() const { return recovered_instance_; }
    absl::Status recordInstance(std::string instance);
    std::optional<Hlc> firstEvent(StoryId story) const;
    bool hasPhysicalPolicy() const override { return physical_policy_; }
    absl::Status flush();
    // Records waiting for the commit loop; tests use it to know a group has formed behind a blocked fsync.
    size_t queuedRecords() const;
    struct CommitStats
    {
        uint64_t syncs{};
        uint64_t records{};
    };
    // Group commits that reached a successful sync, and the records they carried.
    CommitStats commitStats() const;
    absl::Status recordSeal(const Chunk& chunk);
    absl::Status recordSettled(const std::string& chunk_id);
    // Stories whose D record is journaled.
    std::set<StoryId> droppedStories() const;

protected:
    bool capacityReached() const override { return reserve_low_.load() || RamJournal::capacityReached(); }
    bool supportsDurable() const override { return true; }
    bool durableAvailable() const override { return !failed_.load(); }
    void finishAppend(AppendCallback done, absl::StatusOr<std::vector<AppendResult>> results) override;
    void persist(const Event& event, std::function<void(absl::Status)> done) override;
    void beginPersistBatch() override;
    void endPersistBatch() override;
    Hlc reserveFrontier(Hlc frontier) const override;
    absl::StatusOr<int64_t> reservePhysicalFrontier(StoryId story, int64_t frontier) const override;
    absl::Status persistDrop(StoryId story) override;

private:
    struct Write
    {
        std::string bytes;
        std::function<void(absl::Status)> done;
        std::chrono::steady_clock::time_point queued_at{};
    };
    void enqueue(Write write);
    static void flushCollected(WalJournal& journal);
    static thread_local bool collecting_;
    static thread_local std::vector<Write> collected_;
    absl::Status persistRecord(std::string payload);
    void commit();
    // Committer only: samples the WAL file system's free bytes at most every kReserveSampleInterval.
    void sampleReserve();
    uint64_t recover();
    absl::Status rotate();
    void truncate();
    absl::Status reclaim();
    void trackRecord(std::string_view payload, uint64_t segment);
    // Wv2 record: every writer's counters and the dedupe window entries that are acknowledged or rejected.
    std::string writersRecord() const;
    void restoreWriters(std::string_view payload);
    struct Segment
    {
        uint64_t bytes{};
        std::vector<std::pair<StoryId, Hlc>> events;
    };
    std::map<uint64_t, Segment> segments_;
    uint64_t segment_{};
    uint64_t segment_data_bytes_{};
    Hlc persisted_reservation_;
    std::string recovered_instance_, wal_instance_;
    bool physical_policy_{true};
    std::map<StoryId, int64_t> persisted_physical_;


    mutable std::mutex archive_mu_;
    std::map<std::string, SealedChunk> archive_seals_;
    std::set<StoryId> dropped_stories_;
    std::map<StoryId, Hlc> first_events_;
    std::shared_ptr<Clock> clock_;
    WalJournalConfig config_;
    SinkFactory sink_factory_;
    std::unique_ptr<FileSink> sink_;
    int lock_fd_{-1};
    uint64_t bytes_{};
    bool warned_{};
    mutable std::mutex reserve_mu_;
    mutable Hlc reservation_;
    mutable std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<Write> queue_;
    size_t queued_bytes_{};
    bool stopping_{};
    absl::Status failure_;
    std::atomic<bool> failed_{false};
    std::atomic<bool> reserve_low_{false};
    std::chrono::steady_clock::time_point next_reserve_sample_{};
    std::atomic<uint64_t> synced_groups_{0};
    std::atomic<uint64_t> synced_records_{0};
    std::thread committer_;
};

} // namespace chronolog
