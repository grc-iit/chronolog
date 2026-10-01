#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

#include "journal/RamJournal.h"
#include "wal/FileSink.h"

namespace chronolog
{

struct WalJournalConfig
{
    std::string wal_dir{"wal"};
    uint32_t group_commit_window_ms{1};
    size_t group_commit_max_bytes{4u << 20};
    uint32_t reserve_ahead_ms{1000};
    uint64_t wal_max_bytes{1ull << 30};
    uint64_t wal_segment_bytes{64ull << 20};
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
    absl::Status flush();
    absl::Status recordSeal(const Chunk& chunk);
    absl::Status recordSettled(const std::string& chunk_id);

protected:
    bool supportsDurable() const override { return true; }
    bool durableAvailable() const override { return !failed_.load(); }
    void finishAppend(AppendCallback done, absl::StatusOr<std::vector<AppendResult>> results) override;
    void persist(const Event& event, std::function<void(absl::Status)> done) override;
    Hlc reserveFrontier(Hlc frontier) const override;

private:
    struct Write
    {
        std::string bytes;
        std::function<void(absl::Status)> done;
    };
    void enqueue(Write write);
    absl::Status persistRecord(std::string payload);
    void commit();
    uint64_t recover();
    absl::Status rotate();
    void truncate();
    void trackRecord(std::string_view payload, uint64_t segment);
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


    mutable std::mutex archive_mu_;
    std::map<std::string, SealedChunk> archive_seals_;
    std::shared_ptr<Clock> clock_;
    WalJournalConfig config_;
    SinkFactory sink_factory_;
    std::unique_ptr<FileSink> sink_;
    int lock_fd_{-1};
    uint64_t bytes_{};
    bool warned_{};
    mutable std::mutex reserve_mu_;
    mutable Hlc reservation_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<Write> queue_;
    size_t queued_bytes_{};
    bool stopping_{};
    absl::Status failure_;
    std::atomic<bool> failed_{false};
    std::thread committer_;
};

} // namespace chronolog
