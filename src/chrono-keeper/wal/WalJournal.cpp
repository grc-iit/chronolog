#include "wal/WalJournal.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <absl/crc/crc32c.h>
#include "wal/Record.h"
#include "chronolog/internal/v1/internal.pb.h"

namespace chronolog
{
namespace fs = std::filesystem;
namespace
{
void syncDirectory(const fs::path& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0)
        throw std::runtime_error("cannot open WAL directory");
    const int result = ::fsync(fd);
    ::close(fd);
    if(result != 0)
        throw std::runtime_error("cannot sync WAL directory");
}
} // namespace

WalJournal::WalJournal(std::shared_ptr<Clock> clock,
                       std::shared_ptr<const Membership> membership,
                       RamJournalConfig ram_config,
                       WalJournalConfig config,
                       SinkFactory sink_factory)
    : RamJournal(clock, std::move(membership), ram_config)
    , clock_(std::move(clock))
    , config_(std::move(config))
{
    if(config_.wal_dir.empty() || config_.group_commit_max_bytes == 0 || config_.reserve_ahead_ms == 0 ||
       config_.wal_max_bytes == 0)
        throw std::invalid_argument("invalid WAL configuration");
    fs::create_directories(config_.wal_dir);
    lock_fd_ = ::open((fs::path(config_.wal_dir) / "lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if(lock_fd_ < 0)
        throw std::runtime_error("cannot open WAL lock");
    try
    {
        if(::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("WAL directory is already in use");
        const auto sequence = recover();
        sink_ = sink_factory((fs::path(config_.wal_dir) / (std::to_string(sequence) + ".wal")).string());
        for(auto path = fs::absolute(config_.wal_dir); !path.empty(); path = path.parent_path())
        {
            syncDirectory(path);
            if(path == path.root_path())
                break;
        }
        committer_ = std::thread([this] { commit(); });
        (void)reserveFrontier(clock_->tick());
    }
    catch(...)
    {
        if(committer_.joinable())
        {
            {
                std::lock_guard lock(queue_mu_);
                stopping_ = true;
            }
            queue_cv_.notify_all();
            committer_.join();
        }
        ::close(lock_fd_);
        throw;
    }
}

WalJournal::~WalJournal()
{
    {
        std::lock_guard lock(queue_mu_);
        stopping_ = true;
    }
    queue_cv_.notify_all();
    committer_.join();
    sink_.reset();
    ::close(lock_fd_);
}

uint64_t WalJournal::recover()
{
    std::map<uint64_t, fs::path> segments;
    for(const auto& entry: fs::directory_iterator(config_.wal_dir))
    {
        if(entry.path().extension() != ".wal")
            continue;
        const auto stem = entry.path().stem().string();
        uint64_t sequence{};
        const auto parsed = std::from_chars(stem.data(), stem.data() + stem.size(), sequence);
        if(parsed.ec != std::errc{} || parsed.ptr != stem.data() + stem.size() || sequence == UINT64_MAX ||
           !segments.emplace(sequence, entry.path()).second)
            throw std::runtime_error("invalid WAL segment sequence");
    }
    Hlc maximum;
    for(const auto& [sequence, path]: segments)
    {
        std::ifstream stream(path, std::ios::binary);
        if(!stream)
            throw std::runtime_error("cannot read WAL segment");
        const auto size = fs::file_size(path);
        uint64_t offset = 0;
        while(offset < size)
        {
            if(size - offset < 8)
                break;
            char header[8];
            if(!stream.read(header, sizeof(header)))
                throw std::runtime_error("cannot read WAL header");
            const auto length = wal::uint32(std::string_view(header, 4));
            const auto checksum = wal::uint32(std::string_view(header + 4, 4));
            if(length > size - offset - 8)
                break;
            if(length == 0 || length > (256u << 20))
                throw std::runtime_error("invalid WAL record length");
            std::string payload(length, '\0');
            if(!stream.read(payload.data(), length))
                throw std::runtime_error("cannot read WAL payload");
            if(static_cast<uint32_t>(absl::ComputeCrc32c(payload)) != checksum)
            {
                if(offset + 8 + length == size)
                    break;
                throw std::runtime_error("WAL checksum mismatch before tail");
            }
            const auto body = std::string_view(payload).substr(1);
            if(payload.front() == 'E')
            {
                auto event = wal::decode(body);
                restore(event);
                maximum = std::max(maximum, event.hlc);
            }
            else if(payload.front() == 'R')
                reservation_ = std::max(reservation_, wal::decodeReserve(body));
            else if(payload.front() == 'S')
            {
                internal::v1::ChunkIdentity identity;
                if(!identity.ParseFromArray(body.data(), static_cast<int>(body.size())) || identity.chunk_id().empty())
                    throw std::runtime_error("invalid WAL seal");
                Chunk chunk;
                chunk.id = identity.chunk_id();
                chunk.story_id = identity.story_id();
                chunk.start = {identity.start().physical_ns(), identity.start().logical()};
                chunk.end = {identity.end().physical_ns(), identity.end().logical()};
                if(chunk.story_id == 0 || chunk.start >= chunk.end)
                    throw std::runtime_error("invalid WAL seal bounds");
                const auto id = chunk.id;
                archive_seals_[id] = SealedChunk{std::move(chunk), false};
            }
            else if(payload.front() == 'T')
            {
                auto seal = archive_seals_.find(std::string(body));
                if(seal == archive_seals_.end())
                    throw std::runtime_error("settlement without WAL seal");
                seal->second.settled = true;
            }
            else
                throw std::runtime_error("unknown WAL record type");
            offset += 8 + length;
        }
        if(offset != size)
        {
            if(sequence != segments.rbegin()->first)
                throw std::runtime_error("torn WAL record before final segment");
            fs::resize_file(path, offset);
            if(auto status = openFileSink(path.string())->sync(); !status.ok())
                throw std::runtime_error("cannot sync recovered WAL tail: " + std::string(status.message()));
        }
        bytes_ += offset;
    }
    for(const auto& [id, seal]: archive_seals_)
        if(seal.settled)
            eraseEvents(seal.chunk.story_id, {Range::Axis::Hlc, seal.chunk.start, seal.chunk.end}, true);
    if(!segments.empty())
        (void)clock_->observe(std::max(maximum, reservation_));
    return segments.empty() ? 1 : segments.rbegin()->first + 1;
}

void WalJournal::enqueue(Write write)
{
    {
        std::lock_guard lock(queue_mu_);
        queued_bytes_ += write.bytes.size();
        queue_.push_back(std::move(write));
    }
    queue_cv_.notify_one();
}

void WalJournal::finishAppend(AppendCallback done, absl::StatusOr<std::vector<AppendResult>> results)
{
    if(std::this_thread::get_id() == committer_.get_id())
        return done(std::move(results));
    enqueue(Write{{}, [done = std::move(done), results = std::move(results)](absl::Status) mutable {
                      done(std::move(results));
                  }});
}

void WalJournal::persist(const Event& event, std::function<void(absl::Status)> done)
{
    enqueue(Write{wal::frame(wal::encode(event)), std::move(done)});
}

Hlc WalJournal::reserveFrontier(Hlc frontier) const
{
    std::unique_lock lock(reserve_mu_);
    if(frontier < reservation_)
        return frontier;
    Hlc next = frontier;
    const auto ahead = static_cast<int64_t>(config_.reserve_ahead_ms) * 1'000'000;
    next.physical_ns = frontier.physical_ns > INT64_MAX - ahead ? INT64_MAX : frontier.physical_ns + ahead;
    std::promise<absl::Status> promise;
    auto future = promise.get_future();
    const_cast<WalJournal*>(this)->enqueue(Write{wal::frame(wal::reserve(next)), [&promise](absl::Status status) {
                                                     promise.set_value(std::move(status));
                                                 }});
    if(future.get().ok())
        reservation_ = next;
    return std::min(frontier, reservation_);
}

void WalJournal::commit()
{
    for(;;)
    {
        std::vector<Write> group;
        {
            std::unique_lock lock(queue_mu_);
            queue_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if(queue_.empty() && stopping_)
                return;
            const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.group_commit_window_ms);
            queue_cv_.wait_until(lock,
                                 deadline,
                                 [this] { return stopping_ || queued_bytes_ >= config_.group_commit_max_bytes; });
            size_t bytes = 0;
            while(!queue_.empty() &&
                  (group.empty() || bytes + queue_.front().bytes.size() <= config_.group_commit_max_bytes))
            {
                bytes += queue_.front().bytes.size();
                group.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            queued_bytes_ -= bytes;
        }
        absl::Status status = failure_;
        const bool has_records =
                std::any_of(group.begin(), group.end(), [](const Write& write) { return !write.bytes.empty(); });
        if(status.ok() && has_records)
        {
            for(const auto& write: group)
            {
                status = sink_->write(write.bytes);
                if(!status.ok())
                    break;
                bytes_ += write.bytes.size();
            }
            if(status.ok())
                status = sink_->sync();
            if(!status.ok())
            {
                failure_ = status = absl::UnavailableError(std::string(status.message()));
                failed_.store(true);
            }
            if(bytes_ > config_.wal_max_bytes && !warned_)
            {
                warned_ = true;
                std::cerr << "chrono_keeper: WAL exceeds wal_max_bytes; truncation is not enabled\n";
            }
        }
        for(auto& write: group) write.done(status);
    }
}

} // namespace chronolog

namespace chronolog
{
absl::Status WalJournal::persistRecord(std::string payload)
{
    std::promise<absl::Status> promise;
    auto future = promise.get_future();
    enqueue(Write{wal::frame(payload), [&promise](absl::Status status) { promise.set_value(std::move(status)); }});
    return future.get();
}

absl::Status WalJournal::recordSeal(const Chunk& chunk)
{
    internal::v1::ChunkIdentity identity;
    identity.set_chunk_id(chunk.id);
    identity.set_story_id(chunk.story_id);
    identity.mutable_start()->set_physical_ns(chunk.start.physical_ns);
    identity.mutable_start()->set_logical(chunk.start.logical);
    identity.mutable_end()->set_physical_ns(chunk.end.physical_ns);
    identity.mutable_end()->set_logical(chunk.end.logical);
    auto status = persistRecord(std::string(1, 'S') + identity.SerializeAsString());
    if(status.ok())
    {
        std::lock_guard lock(archive_mu_);
        Chunk metadata = chunk;
        metadata.events.clear();
        archive_seals_[chunk.id] = SealedChunk{std::move(metadata), false};
    }
    return status;
}

absl::Status WalJournal::recordSettled(const std::string& chunk_id)
{
    auto status = persistRecord(std::string(1, 'T') + chunk_id);
    if(status.ok())
    {
        std::lock_guard lock(archive_mu_);
        archive_seals_.at(chunk_id).settled = true;
    }
    return status;
}

std::vector<WalJournal::SealedChunk> WalJournal::sealedChunks() const
{
    std::lock_guard lock(archive_mu_);
    std::vector<SealedChunk> out;
    for(const auto& [id, seal]: archive_seals_) out.push_back(seal);
    return out;
}
} // namespace chronolog
