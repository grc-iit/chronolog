#include "wal/WalJournal.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
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
    , sink_factory_(std::move(sink_factory))
{
    if(config_.wal_dir.empty() || config_.group_commit_max_bytes == 0 || config_.reserve_ahead_ms == 0 ||
       config_.wal_max_bytes == 0 || config_.wal_segment_bytes == 0)
        throw std::invalid_argument("invalid WAL configuration");
    fs::create_directories(config_.wal_dir);
    lock_fd_ = ::open((fs::path(config_.wal_dir) / "lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if(lock_fd_ < 0)
        throw std::runtime_error("cannot open WAL lock");
    try
    {
        if(::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
            throw std::runtime_error("WAL directory is already in use");
        segment_ = recover() - 1;
        if(auto status = rotate(); !status.ok())
            throw std::runtime_error(std::string(status.message()));
        truncate();
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
            else if(payload.front() == 'W')
                restoreWriters(body);
            else if(payload.front() != 'S' && payload.front() != 'T')
                throw std::runtime_error("unknown WAL record type");
            trackRecord(payload, sequence);
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
        segments_[sequence].bytes = offset;
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
            try
            {
                for(const auto& write: group)
                {
                    if(!status.ok())
                        break;
                    if(!write.bytes.empty() && segment_data_bytes_ != 0 &&
                       segments_[segment_].bytes + write.bytes.size() > config_.wal_segment_bytes)
                    {
                        status = sink_->sync();
                        if(status.ok())
                            status = rotate();
                        if(!status.ok())
                            break;
                    }
                    status = sink_->write(write.bytes);
                    if(!status.ok())
                        break;
                    bytes_ += write.bytes.size();
                    segments_[segment_].bytes += write.bytes.size();
                    segment_data_bytes_ += write.bytes.size();
                    if(!write.bytes.empty())
                        trackRecord(std::string_view(write.bytes).substr(8), segment_);
                }
                if(status.ok())
                    status = sink_->sync();
                const bool settled =
                        std::any_of(group.begin(),
                                    group.end(),
                                    [](const Write& write) { return write.bytes.size() > 8 && write.bytes[8] == 'T'; });
                if(status.ok() && settled)
                {
                    for(auto& write: group)
                        if(write.bytes.size() > 8 && write.bytes[8] == 'E')
                        {
                            write.done(status);
                            write.done = {};
                        }
                    status = rotate();
                    if(status.ok())
                        truncate();
                }
            }
            catch(const std::exception& error)
            {
                status = absl::UnavailableError(error.what());
            }
            if(!status.ok())
            {
                failure_ = status = absl::UnavailableError(std::string(status.message()));
                failed_.store(true);
            }
            if(bytes_ > config_.wal_max_bytes && !warned_)
            {
                warned_ = true;
                std::cerr << "chrono_keeper: WAL exceeds wal_max_bytes; unsettled events remain protected\n";
            }
        }
        for(auto& write: group)
            if(write.done)
                write.done(status);
    }
}

} // namespace chronolog

namespace chronolog
{
absl::Status WalJournal::flush()
{
    std::promise<absl::Status> promise;
    auto future = promise.get_future();
    enqueue(Write{{}, [&promise](absl::Status status) { promise.set_value(std::move(status)); }});
    return future.get();
}

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
    return persistRecord(std::string(1, 'S') + identity.SerializeAsString());
}

absl::Status WalJournal::recordSettled(const std::string& chunk_id)
{
    {
        std::lock_guard lock(archive_mu_);
        if(!archive_seals_.contains(chunk_id))
            return absl::NotFoundError("unknown sealed chunk");
    }
    return persistRecord(std::string(1, 'T') + chunk_id);
}

std::vector<WalJournal::SealedChunk> WalJournal::sealedChunks() const
{
    std::lock_guard lock(archive_mu_);
    std::vector<SealedChunk> out;
    for(const auto& [id, seal]: archive_seals_) out.push_back(seal);
    return out;
}
} // namespace chronolog

namespace chronolog
{
std::string WalJournal::writersRecord() const
{
    std::ostringstream out;
    out << 'W';
    const auto writers = checkpointWriters();
    out << writers.size() << '\n';
    for(const auto& writer: writers)
    {
        out << writer.key.story_id << ' ' << writer.key.writer_id << ' ' << writer.key.incarnation << ' '
            << writer.next_sequence << ' ' << writer.last_hlc.physical_ns << ' ' << writer.last_hlc.logical << ' '
            << writer.released << ' ' << writer.assigned << ' ' << writer.window.size() << '\n';
        for(const auto& result: writer.window)
            out << result.id.sequence << ' ' << result.hlc.physical_ns << ' ' << result.hlc.logical << '\n';
    }
    return out.str();
}

void WalJournal::restoreWriters(std::string_view payload)
{
    std::istringstream in{std::string(payload)};
    size_t count{};
    if(!(in >> count) || count > payload.size())
        throw std::runtime_error("invalid WAL writers");
    for(size_t i = 0; i < count; ++i)
    {
        WriterCheckpoint writer;
        size_t window{};
        if(!(in >> writer.key.story_id >> writer.key.writer_id >> writer.key.incarnation >> writer.next_sequence >>
             writer.last_hlc.physical_ns >> writer.last_hlc.logical >> writer.released >> writer.assigned >> window) ||
           writer.key.story_id == 0 || writer.key.writer_id == 0 || writer.key.incarnation == 0 ||
           writer.next_sequence == 0 || window > payload.size())
            throw std::runtime_error("invalid WAL writer checkpoint");
        for(size_t j = 0; j < window; ++j)
        {
            AppendResult result;
            result.id = {writer.key.story_id, writer.key.writer_id, writer.key.incarnation, 0};
            result.achieved = Durability::Durable;
            if(!(in >> result.id.sequence >> result.hlc.physical_ns >> result.hlc.logical) || result.id.sequence == 0 ||
               result.id.sequence >= writer.next_sequence)
                throw std::runtime_error("invalid WAL dedupe checkpoint");
            writer.window.push_back(result);
        }
        restoreWriter(writer);
        (void)clock_->observe(writer.last_hlc);
    }
    in >> std::ws;
    if(!in.eof())
        throw std::runtime_error("trailing WAL writer checkpoint bytes");
}

void WalJournal::trackRecord(std::string_view payload, uint64_t segment)
{
    const auto body = payload.substr(1);
    if(payload.front() == 'E')
    {
        const auto event = wal::decode(body);
        segments_[segment].events.emplace_back(event.id.story_id, event.hlc);
    }
    else if(payload.front() == 'R')
        persisted_reservation_ = std::max(persisted_reservation_, wal::decodeReserve(body));
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
        std::lock_guard lock(archive_mu_);
        const auto id = chunk.id;
        const bool settled = archive_seals_.contains(id) && archive_seals_.at(id).settled;
        archive_seals_[id] = SealedChunk{std::move(chunk), settled};
    }
    else if(payload.front() == 'T')
    {
        std::lock_guard lock(archive_mu_);
        auto seal = archive_seals_.find(std::string(body));
        if(seal == archive_seals_.end())
            throw std::runtime_error("settlement without WAL seal");
        seal->second.settled = true;
    }
}

absl::Status WalJournal::rotate()
{
    const auto next = segment_ + 1;
    const auto path = fs::path(config_.wal_dir) / (std::to_string(next) + ".wal");
    const auto temporary = path.string() + ".partial";
    fs::remove(temporary);
    auto sink = sink_factory_(temporary);
    uint64_t size = 0;
    auto write = [&](const std::string& payload)
    {
        const auto bytes = wal::frame(payload);
        size += bytes.size();
        return sink->write(bytes);
    };
    auto status = write(writersRecord());
    if(status.ok())
        status = write(wal::reserve(persisted_reservation_));
    for(const auto& seal: sealedChunks())
    {
        if(!status.ok())
            break;
        internal::v1::ChunkIdentity identity;
        identity.set_chunk_id(seal.chunk.id);
        identity.set_story_id(seal.chunk.story_id);
        identity.mutable_start()->set_physical_ns(seal.chunk.start.physical_ns);
        identity.mutable_start()->set_logical(seal.chunk.start.logical);
        identity.mutable_end()->set_physical_ns(seal.chunk.end.physical_ns);
        identity.mutable_end()->set_logical(seal.chunk.end.logical);
        status = write(std::string(1, 'S') + identity.SerializeAsString());
        if(status.ok() && seal.settled)
            status = write(std::string(1, 'T') + seal.chunk.id);
    }
    if(status.ok())
        status = sink->sync();
    if(!status.ok())
        return status;
    fs::rename(temporary, path);
    syncDirectory(config_.wal_dir);
    sink_ = std::move(sink);
    segment_ = next;
    segments_[segment_].bytes = size;
    bytes_ += size;
    segment_data_bytes_ = 0;
    return absl::OkStatus();
}

void WalJournal::truncate()
{
    const auto seals = sealedChunks();
    for(auto it = segments_.begin(); it != segments_.end();)
    {
        if(it->first == segment_)
            break;
        const bool settled = std::all_of(it->second.events.begin(),
                                         it->second.events.end(),
                                         [&](const auto& event)
                                         {
                                             return std::any_of(seals.begin(),
                                                                seals.end(),
                                                                [&](const SealedChunk& seal)
                                                                {
                                                                    return seal.settled &&
                                                                           seal.chunk.story_id == event.first &&
                                                                           seal.chunk.start <= event.second &&
                                                                           event.second < seal.chunk.end;
                                                                });
                                         });
        if(!settled)
        {
            ++it;
            continue;
        }
        fs::remove(fs::path(config_.wal_dir) / (std::to_string(it->first) + ".wal"));
        bytes_ -= it->second.bytes;
        it = segments_.erase(it);
    }
    syncDirectory(config_.wal_dir);
}
} // namespace chronolog
