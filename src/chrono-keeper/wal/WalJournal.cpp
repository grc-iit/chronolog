#include "wal/WalJournal.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <absl/crc/crc32c.h>
#include <absl/log/log.h>
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
            if(sequence == segments.begin()->first && offset == 0)
                physical_policy_ = payload.front() == 'Q' && payload.substr(1) == "1";
            const auto body = std::string_view(payload).substr(1);
            if(payload.front() == 'E')
            {
                auto event = wal::decode(body);
                restore(event);
                maximum = std::max(maximum, event.hlc);
            }
            else if(payload.front() == 'R')
                reservation_ = std::max(reservation_, wal::decodeReserve(body));
            else if(payload.front() == 'I')
                recovered_instance_ = wal_instance_ = std::string(body);
            else if(payload.front() == 'W')
                restoreWriters(body);
            else if(payload.front() == 'D')
                restoreDrop(wal::decodeDrop(body));
            else if(payload.front() != 'S' && payload.front() != 'T' && payload.front() != 'Q' &&
                    payload.front() != 'F' && payload.front() != 'B')
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
        // ACCEPTED data in a recovered seal may exist only in the archive, even before receipt settlement.
        eraseEvents(seal.chunk.story_id,
                    {Range::Axis::Hlc, seal.settled ? seal.chunk.start : seal.chunk.end, seal.chunk.end},
                    true);
    {
        std::lock_guard lock(archive_mu_);
        for(auto story: dropped_stories_) eraseEvents(story, {Range::Axis::Hlc, {}, {INT64_MAX, UINT32_MAX}});
    }
    if(!segments.empty())
    {
        const auto restart = std::max(maximum, reservation_);
        clock_->observeFloor(restart);
        const auto ahead = static_cast<int64_t>(config_.reserve_ahead_ms) * 1'000'000;
        clock_->raiseAcceptanceClock(restart.physical_ns < INT64_MIN + ahead ? INT64_MIN : restart.physical_ns - ahead);
        for(const auto& [story, frontier]: persisted_physical_)
        {
            (void)story;
            const auto window = PhysicalPolicy{}.acceptance_window_ns;
            clock_->raiseAcceptanceClock(frontier > INT64_MAX - window ? INT64_MAX : frontier + window);
        }
    }
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
    const bool rejected =
            results.ok() && std::any_of(results->begin(),
                                        results->end(),
                                        [](const AppendResult& result) { return absl::IsOutOfRange(result.status); });
    if(!rejected)
        return done(std::move(results));
    auto bytes = rejected ? wal::frame(writersRecord()) : std::string{};
    enqueue(Write{std::move(bytes),
                  [done = std::move(done), results = std::move(results), rejected](absl::Status status) mutable
                  {
                      if(rejected && !status.ok())
                          return done(status);
                      done(std::move(results));
                  }});
}

thread_local bool WalJournal::collecting_ = false;
thread_local std::vector<WalJournal::Write> WalJournal::collected_;

void WalJournal::persist(const Event& event, std::function<void(absl::Status)> done)
{
    Write write{wal::frame(wal::encode(event)), std::move(done)};
    if(collecting_)
        collected_.push_back(std::move(write));
    else
        enqueue(std::move(write));
}

void WalJournal::beginPersistBatch() { collecting_ = true; }

void WalJournal::endPersistBatch()
{
    collecting_ = false;
    flushCollected(*this);
}

// One lock and one wake-up for every record of the batch.
void WalJournal::flushCollected(WalJournal& journal)
{
    if(collected_.empty())
        return;
    {
        std::lock_guard lock(journal.queue_mu_);
        for(auto& write: collected_)
        {
            journal.queued_bytes_ += write.bytes.size();
            journal.queue_.push_back(std::move(write));
        }
    }
    collected_.clear();
    journal.queue_cv_.notify_one();
}

Hlc WalJournal::reserveFrontier(Hlc frontier) const
{
    std::unique_lock lock(reserve_mu_);
    if(frontier < reservation_)
        return frontier;
    // Records of the batch being assembled on this thread keep their place ahead of the reservation.
    flushCollected(*const_cast<WalJournal*>(this));
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
                const bool settled = std::any_of(group.begin(),
                                                 group.end(),
                                                 [](const Write& write) {
                                                     return write.bytes.size() > 8 &&
                                                            (write.bytes[8] == 'T' || write.bytes[8] == 'D');
                                                 });
                if(status.ok() && settled)
                {
                    for(auto& write: group)
                        if(write.bytes.size() > 8 && write.bytes[8] == 'E')
                        {
                            write.done(status);
                            write.done = {};
                        }
                    status = reclaim();
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
                LOG(WARNING) << "WAL exceeds wal_max_bytes; unsettled events remain protected";
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
size_t WalJournal::queuedRecords() const
{
    std::lock_guard lock(queue_mu_);
    return queue_.size();
}

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
    auto events = read(chunk.story_id, {Range::Axis::Hlc, chunk.start, chunk.end});
    if(!events.ok())
        return events.status();
    if(!events->empty())
    {
        const auto first = events->front().hlc;
        auto known = firstEvent(chunk.story_id);
        if(!known || first < *known)
            if(auto status = persistRecord("B" + std::to_string(chunk.story_id) + " " +
                                           std::to_string(first.physical_ns) + " " + std::to_string(first.logical));
               !status.ok())
                return status;
    }
    internal::v1::ChunkIdentity identity;
    identity.set_physical_policy(physical_policy_);
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

absl::Status WalJournal::persistDrop(StoryId story) { return persistRecord(wal::drop(story)); }

std::set<StoryId> WalJournal::droppedStories() const
{
    std::lock_guard lock(archive_mu_);
    return dropped_stories_;
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
std::string WalJournal::writersRecord() const { return "W" + checkpointText(); }

void WalJournal::restoreWriters(std::string_view payload)
{
    std::istringstream in{std::string(payload)};
    const bool version3 = payload.starts_with("v3 ");
    const bool version2 = payload.starts_with("v2 ") || version3;
    if(version2)
        in.ignore(3);
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
            if(version2)
            {
                int code{};
                if(!(in >> code) || (code != 0 && code != static_cast<int>(absl::StatusCode::kOutOfRange)))
                    throw std::runtime_error("invalid WAL rejection code");
                if(code != 0)
                {
                    result.status = absl::OutOfRangeError("physical reading outside acceptance window");
                    result.achieved = Durability::Unspecified;
                }
            }
            if(version3)
            {
                uint32_t rejection{};
                if(!(in >> rejection) || rejection > static_cast<uint32_t>(AppendRejection::FencedOwnerRemoved))
                    throw std::runtime_error("invalid WAL append rejection");
                result.rejection = static_cast<AppendRejection>(rejection);
                if(!absl::IsFailedPrecondition(result.status) && result.rejection != AppendRejection::Unspecified)
                    throw std::runtime_error("WAL append rejection disagrees with status");
            }
            writer.window.push_back(result);
        }
        restoreWriter(writer);
        clock_->observeFloor(writer.last_hlc);
    }
    in >> std::ws;
    if(!in.eof())
        throw std::runtime_error("trailing WAL writer checkpoint bytes");
}

void WalJournal::trackRecord(std::string_view payload, uint64_t segment)
{
    const auto body = payload.substr(1);
    if(payload.front() == 'I')
        wal_instance_ = std::string(body);
    else if(payload.front() == 'E')
    {
        const auto event = wal::decode(body);
        segments_[segment].events.emplace_back(event.id.story_id, event.hlc);
        std::lock_guard lock(archive_mu_);
        auto [it, inserted] = first_events_.try_emplace(event.id.story_id, event.hlc);
        if(!inserted)
            it->second = std::min(it->second, event.hlc);
    }
    else if(payload.front() == 'B')
    {
        std::istringstream in{std::string(body)};
        StoryId story;
        Hlc first;
        if(!(in >> story >> first.physical_ns >> first.logical))
            throw std::runtime_error("invalid first event record");
        std::lock_guard lock(archive_mu_);
        auto [it, inserted] = first_events_.try_emplace(story, first);
        if(!inserted)
            it->second = std::min(it->second, first);
    }
    else if(payload.front() == 'R')
        persisted_reservation_ = std::max(persisted_reservation_, wal::decodeReserve(body));
    else if(payload.front() == 'F')
    {
        std::istringstream in{std::string(body)};
        StoryId story{};
        int64_t frontier{};
        if(!(in >> story >> frontier) || story == 0)
            throw std::runtime_error("invalid physical reservation");
        auto [it, inserted] = persisted_physical_.try_emplace(story, frontier);
        if(!inserted)
            it->second = std::max(it->second, frontier);
    }
    else if(payload.front() == 'S')
    {
        internal::v1::ChunkIdentity identity;
        if(!identity.ParseFromArray(body.data(), static_cast<int>(body.size())) || identity.chunk_id().empty())
            throw std::runtime_error("invalid WAL seal");
        Chunk chunk;
        chunk.physical_policy = identity.physical_policy();
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
    else if(payload.front() == 'D')
    {
        std::lock_guard lock(archive_mu_);
        dropped_stories_.insert(wal::decodeDrop(body));
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
    auto status = write(physical_policy_ ? "Q1" : "Q0");
    if(status.ok() && !wal_instance_.empty())
        status = write("I" + wal_instance_);
    {
        std::lock_guard lock(archive_mu_);
        for(const auto& [story, first]: first_events_)
            if(status.ok())
                status = write("B" + std::to_string(story) + " " + std::to_string(first.physical_ns) + " " +
                               std::to_string(first.logical));
    }
    if(status.ok())
        status = write(writersRecord());
    for(const auto story: droppedStories())
        if(status.ok())
            status = write(wal::drop(story));
    for(const auto& [story, frontier]: persisted_physical_)
        if(status.ok())
            status = write("F" + std::to_string(story) + " " + std::to_string(frontier));
    if(status.ok())
        status = write(wal::reserve(persisted_reservation_));
    for(const auto& seal: sealedChunks())
    {
        if(!status.ok())
            break;
        internal::v1::ChunkIdentity identity;
        identity.set_physical_policy(seal.chunk.physical_policy);
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

namespace
{
// Settled seals per story ordered by start, with the running maximum of their ends: an event is covered when some
// seal that starts at or before it ends after it, which one binary search decides.
class SettledIndex
{
public:
    SettledIndex(const std::vector<WalJournal::SealedChunk>& seals, std::set<StoryId> dropped)
        : dropped_(std::move(dropped))
    {
        std::map<StoryId, std::vector<std::pair<Hlc, Hlc>>> ranges;
        for(const auto& seal: seals)
            if(seal.settled)
                ranges[seal.chunk.story_id].emplace_back(seal.chunk.start, seal.chunk.end);
        for(auto& [story, list]: ranges)
        {
            std::sort(list.begin(), list.end());
            auto& entry = stories_[story];
            for(const auto& [start, end]: list)
            {
                entry.starts.push_back(start);
                entry.max_ends.push_back(entry.max_ends.empty() ? end : std::max(entry.max_ends.back(), end));
            }
        }
    }
    // A D record settles every record of its story.
    bool covers(StoryId story, Hlc hlc) const
    {
        if(dropped_.contains(story))
            return true;
        const auto found = stories_.find(story);
        if(found == stories_.end())
            return false;
        const auto& starts = found->second.starts;
        const auto count = static_cast<size_t>(std::upper_bound(starts.begin(), starts.end(), hlc) - starts.begin());
        return count != 0 && hlc < found->second.max_ends[count - 1];
    }
    template <typename Events>
    bool coversAll(const Events& events) const
    {
        return std::all_of(events.begin(),
                           events.end(),
                           [&](const auto& event) { return covers(event.first, event.second); });
    }

private:
    struct Entry
    {
        std::vector<Hlc> starts, max_ends;
    };
    std::map<StoryId, Entry> stories_;
    std::set<StoryId> dropped_;
};
} // namespace

void WalJournal::truncate()
{
    const SettledIndex settled(sealedChunks(), droppedStories());
    bool removed = false;
    for(auto it = segments_.begin(); it != segments_.end();)
    {
        if(it->first == segment_)
            break;
        if(!settled.coversAll(it->second.events))
        {
            ++it;
            continue;
        }
        fs::remove(fs::path(config_.wal_dir) / (std::to_string(it->first) + ".wal"));
        bytes_ -= it->second.bytes;
        it = segments_.erase(it);
        removed = true;
    }
    if(removed)
        syncDirectory(config_.wal_dir);
}

absl::Status WalJournal::reclaim()
{
    truncate();
    // Rotating writes a checkpoint of every writer, so it pays only when it lets the whole active segment go. While
    // the active segment still holds unsettled events a rotation frees nothing, and the size trigger in commit()
    // rotates it long before the WAL grows beyond wal_segment_bytes.
    const auto& active = segments_[segment_].events;
    if(active.empty() || !SettledIndex(sealedChunks(), droppedStories()).coversAll(active))
        return absl::OkStatus();
    auto status = rotate();
    if(status.ok())
        truncate();
    return status;
}
} // namespace chronolog

namespace chronolog
{
absl::StatusOr<int64_t> WalJournal::reservePhysicalFrontier(StoryId story, int64_t frontier) const
{
    frontier = std::min(frontier, reserveFrontier(Hlc{frontier, 0}).physical_ns);
    flushCollected(*const_cast<WalJournal*>(this));
    std::lock_guard lock(reserve_mu_);
    auto status =
            const_cast<WalJournal*>(this)->persistRecord("F" + std::to_string(story) + " " + std::to_string(frontier));
    if(!status.ok())
        return status;
    return frontier;
}
} // namespace chronolog

namespace chronolog
{
absl::Status WalJournal::recordInstance(std::string instance) { return persistRecord("I" + instance); }
} // namespace chronolog

namespace chronolog
{
std::optional<Hlc> WalJournal::firstEvent(StoryId story) const
{
    std::lock_guard lock(archive_mu_);
    auto it = first_events_.find(story);
    return it == first_events_.end() ? std::nullopt : std::optional<Hlc>{it->second};
}
} // namespace chronolog
