#include <sys/statvfs.h>
#include <absl/log/log.h>
#include "tier/FileTierStore.h"
#include <absl/crc/crc32c.h>
#include "tier/ArchiveReaderPool.h"
#include "tier/FileIO.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <future>
#include <fstream>
#include <nlohmann/json.hpp>
#include <deque>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <tuple>

namespace chronolog
{
namespace
{
absl::StatusOr<size_t> ReaderThreads(size_t configured)
{
    constexpr size_t cap = 8;
    if(!configured)
    {
        if(const char* value = std::getenv("CHRONOLOG_ARCHIVE_READ_THREADS"))
        {
            const auto end = value + std::char_traits<char>::length(value);
            const auto parsed = std::from_chars(value, end, configured);
            if(parsed.ec != std::errc{} || parsed.ptr != end || !configured)
                return absl::InvalidArgumentError("CHRONOLOG_ARCHIVE_READ_THREADS must be a positive integer");
        }
        else
            configured = std::max(1u, std::thread::hardware_concurrency());
    }
    return std::min(configured, cap);
}

std::string Hex(const std::string& input)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    for(unsigned char c: input)
    {
        output += digits[c >> 4];
        output += digits[c & 15];
    }
    return output;
}

absl::StatusOr<std::string> Unhex(const std::string& input)
{
    if(input.size() % 2 != 0)
        return absl::InvalidArgumentError("invalid chunk name");
    std::string output;
    for(std::size_t i = 0; i < input.size(); i += 2)
    {
        unsigned byte = 0;
        const auto parsed = std::from_chars(input.data() + i, input.data() + i + 2, byte, 16);
        if(parsed.ec != std::errc{} || parsed.ptr != input.data() + i + 2)
            return absl::InvalidArgumentError("invalid chunk name");
        output += static_cast<char>(byte);
    }
    return output;
}

std::string Filename(const Chunk& chunk, const std::string& writer, const std::string& extension)
{
    return std::to_string(chunk.start.physical_ns) + "_" + std::to_string(chunk.start.logical) + "_" +
           std::to_string(chunk.end.physical_ns) + "_" + std::to_string(chunk.end.logical) + "_" +
           (chunk.exempt ? "1_" : "0_") + std::to_string(chunk.events.size()) + "_" + Hex(writer) + "_" +
           Hex(chunk.id) + (chunk.physical_policy ? "_1" : "_0") + extension;
}

template <typename T>
bool Number(const std::string& text, T& value)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

FileChecksum Checksum(const ChunkBytes& bytes)
{
    return {bytes.size,
            static_cast<uint32_t>(absl::ComputeCrc32c(
                    absl::string_view(reinterpret_cast<const char*>(bytes.data.get()), bytes.size)))};
}

absl::StatusOr<FileChecksum> FileChecksumOf(int fd)
{
    FileChecksum result;
    absl::crc32c_t crc{0};
    char buffer[65536];
    for(;;)
    {
        const auto count = ::pread(fd, buffer, sizeof(buffer), static_cast<off_t>(result.bytes));
        if(count < 0 && errno == EINTR)
            continue;
        if(count < 0)
            return tier_detail::IoError("read archive checksum");
        if(count == 0)
            break;
        crc = absl::ExtendCrc32c(crc, absl::string_view(buffer, static_cast<size_t>(count)));
        result.bytes += static_cast<uint64_t>(count);
    }
    result.crc32c = static_cast<uint32_t>(crc);
    return result;
}

absl::Status VerifyChecksum(const ChunkBytes& bytes, std::optional<FileChecksum> expected)
{
    if(expected)
    {
        if(bytes.size != expected->bytes || Checksum(bytes).crc32c != expected->crc32c)
            return absl::UnavailableError("archive file checksum or length mismatch");
    }
    return absl::OkStatus();
}

absl::StatusOr<ManifestRecord> FromFilename(const std::filesystem::path& relative)
{
    ManifestRecord record;
    if((relative.extension() != ".pb" && relative.extension() != ".h5") ||
       !Number(relative.parent_path().string(), record.story_id) || !record.story_id)
        return absl::InvalidArgumentError("not a published chunk");
    std::istringstream name(relative.stem().string());
    std::vector<std::string> fields;
    std::string field;
    while(std::getline(name, field, '_')) fields.push_back(field);
    int exempt = 0;
    if((fields.size() != 8 && fields.size() != 9) || !Number(fields[0], record.start.physical_ns) ||
       !Number(fields[1], record.start.logical) || !Number(fields[2], record.end.physical_ns) ||
       !Number(fields[3], record.end.logical) || !Number(fields[4], exempt) || (exempt != 0 && exempt != 1) ||
       !Number(fields[5], record.event_count) || record.start >= record.end)
        return absl::InvalidArgumentError("not a published chunk");
    auto writer = Unhex(fields[6]);
    auto chunk = Unhex(fields[7]);
    if(!writer.ok() || !chunk.ok() || writer->empty() || chunk->empty())
        return absl::InvalidArgumentError("not a published chunk");
    if(writer->size() > 64 || !std::all_of(writer->begin(),
                                           writer->end(),
                                           [](unsigned char c) {
                                               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                                      (c >= '0' && c <= '9') || c == '-' || c == '_';
                                           }))
        return absl::InvalidArgumentError("invalid chunk writer");
    record.manifest_writer = *writer;
    record.chunk_id = *chunk;
    record.exempt = exempt != 0;
    if(fields.size() == 9)
    {
        if(fields[8] != "0" && fields[8] != "1")
            return absl::InvalidArgumentError("invalid physical policy marker");
        record.physical_policy = fields[8] == "1";
    }
    record.file = relative.generic_string();
    record.state = record.event_count ? ManifestState::Published : ManifestState::Empty;
    return record;
}

absl::Status ValidChunk(const Chunk& chunk)
{
    if(!chunk.story_id || chunk.id.empty() || chunk.id.size() > 128 || chunk.start >= chunk.end ||
       chunk.events.size() > 65536)
        return absl::InvalidArgumentError("invalid chunk identity or window");
    for(const auto& event: chunk.events)
    {
        if(event.id.story_id != chunk.story_id || !event.id.writer_id || !event.id.incarnation || !event.id.sequence ||
           event.hlc < chunk.start || event.hlc >= chunk.end || event.envelope.payload.size() > 1024 * 1024 ||
           (!event.envelope.trace_id.empty() && event.envelope.trace_id.size() != 16) ||
           (!event.envelope.span_id.empty() && event.envelope.span_id.size() != 8))
            return absl::InvalidArgumentError("invalid chunk event");
    }
    return absl::OkStatus();
}

bool SameEvent(const Event& a, const Event& b)
{
    return a.id == b.id && a.hlc == b.hlc && a.durability == b.durability &&
           a.physical.physical_ns == b.physical.physical_ns && a.physical.status == b.physical.status &&
           a.physical.uncertainty_ns == b.physical.uncertainty_ns &&
           std::tie(a.envelope.content_type,
                    a.envelope.payload,
                    a.envelope.trace_id,
                    a.envelope.span_id,
                    a.envelope.attributes) == std::tie(b.envelope.content_type,
                                                       b.envelope.payload,
                                                       b.envelope.trace_id,
                                                       b.envelope.span_id,
                                                       b.envelope.attributes);
}

absl::Status ValidRange(const Range& range)
{
    if(range.axis != Range::Axis::Hlc && range.axis != Range::Axis::Physical)
        return absl::InvalidArgumentError("invalid range axis");
    if(range.axis == Range::Axis::Hlc ? range.start >= range.end : range.start.physical_ns >= range.end.physical_ns)
        return absl::InvalidArgumentError("invalid range bounds");
    return absl::OkStatus();
}

int StateRank(ManifestState state)
{
    if(state == ManifestState::Deleted)
        return 3;
    if(state == ManifestState::Lost)
        return 2;
    if(state == ManifestState::Published || state == ManifestState::Empty)
        return 1;
    return 0;
}

bool Bounded(TimeReading reading)
{
    return reading.status == ClockStatus::Synced && reading.uncertainty_ns &&
           *reading.uncertainty_ns <= PhysicalPolicy{}.uncertainty_cap_ns;
}

std::optional<PhysicalBounds> BoundsOf(std::span<const Event> events)
{
    if(events.empty())
        return std::nullopt;
    PhysicalBounds bounds{std::numeric_limits<int64_t>::max(), std::numeric_limits<int64_t>::min(), false};
    auto saturate = [](__int128_t value)
    {
        return static_cast<int64_t>(std::clamp<__int128_t>(value,
                                                           std::numeric_limits<int64_t>::min(),
                                                           std::numeric_limits<int64_t>::max()));
    };
    for(const auto& event: events)
    {
        const bool bounded = Bounded(event.physical);
        bounds.unbounded |= !bounded;
        const __int128_t p = event.physical.physical_ns;
        const __int128_t u = bounded ? *event.physical.uncertainty_ns : 0;
        bounds.min_lo = std::min(bounds.min_lo, saturate(p - u));
        bounds.max_hi = std::max(bounds.max_hi, saturate(p + u));
    }
    return bounds;
}

bool MayIntersect(const ManifestIndex& index, const ManifestRecord& record, Range range)
{
    const auto found = index.physical_bounds.find(record.file);
    if(found == index.physical_bounds.end())
        return true;
    const auto& file = found->second;
    if(file.story_id != record.story_id || file.start != record.start || file.end != record.end ||
       file.event_count != record.event_count || file.bounds.unbounded)
        return true;
    return file.bounds.min_lo < range.end.physical_ns && file.bounds.max_hi >= range.start.physical_ns;
}

std::string Stem(const std::string& file) { return std::filesystem::path(file).replace_extension().generic_string(); }

std::string RandomOp()
{
    std::random_device random;
    const uint64_t value = (uint64_t{random()} << 32) ^ random() ^
                           static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}
absl::StatusOr<ChunkBytes> LoadTierBytes(const std::shared_ptr<PosixTier>& tier,
                                         const std::shared_ptr<TierDirectory>& directory,
                                         const std::string& file)
{
    auto status = tier->verify(*directory);
    if(!status.ok())
        return status;
    auto data = PosixTier::read(directory->fd.get(), file);
    if(!data.ok())
        return data.status();
    ChunkBytes bytes{std::make_unique<unsigned char[]>(data->size()), data->size()};
    std::copy(data->begin(), data->end(), bytes.data.get());
    return bytes;
}
} // namespace

FileTierStore::FileTierStore(std::filesystem::path root,
                             std::string writer,
                             std::unique_ptr<ManifestLog> log,
                             std::map<StoryId, Hlc> anchors,
                             std::shared_ptr<const ChunkCodec> codec,
                             Unlink unlink,
                             LoadFile load_file,
                             size_t read_threads,
                             DecodeFile decode_file)
    : root_(std::move(root))
    , writer_(std::move(writer))
    , log_(std::move(log))
    , codec_(std::move(codec))
    , unlink_(std::move(unlink))
    , load_file_(std::move(load_file))
    , decode_file_(std::move(decode_file))
    , read_threads_(read_threads)
{
    for(const auto& [story, anchor]: anchors) anchors_[story] = anchor;
}

FileTierStore::~FileTierStore()
{
    for(const auto& [name, tier]: tiers_) tier->stop();
}

absl::StatusOr<std::unique_ptr<FileTierStore>> FileTierStore::Open(std::filesystem::path root,
                                                                   std::string writer,
                                                                   std::map<StoryId, Hlc> anchors,
                                                                   std::shared_ptr<const ChunkCodec> codec,
                                                                   Unlink unlink,
                                                                   LoadFile load_file,
                                                                   size_t read_threads,
                                                                   DecodeFile decode_file,
                                                                   Hooks hooks,
                                                                   TierChain chain)
{
    if(!codec || anchors.contains(0))
        return absl::InvalidArgumentError("invalid tier configuration");
    const auto threads = ReaderThreads(read_threads);
    if(!threads.ok())
        return threads.status();
    if(!unlink)
        unlink = [](const std::filesystem::path& path) { return ::unlink(path.c_str()); };
    if(!load_file)
        load_file = LoadChunkFile;
    if(!decode_file)
        decode_file = DecodeChunkFile;
    if(!chain.deployment_id.empty())
    {
        tier_detail::Fd marker(::open(root.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if(marker.get() < 0)
            return tier_detail::IoError("open local tier marker root");
        auto bytes = PosixTier::read(marker.get(), ".chronolog-tier.json");
        if(!bytes.ok())
            return bytes.status();
        try
        {
            auto identity = nlohmann::json::parse(*bytes);
            if(identity.at("deployment_id").get<std::string>() != chain.deployment_id ||
               identity.at("name").get<std::string>() != "local" || identity.at("rank").get<uint32_t>() != 0 ||
               identity.at("kind").get<std::string>() != "posix")
                return absl::FailedPreconditionError("local tier marker refused");
        }
        catch(const std::exception& error)
        {
            return absl::FailedPreconditionError(error.what());
        }
    }
    else if(!chain.tiers.empty())
        return absl::InvalidArgumentError("tier chain requires deployment identity");
    auto log = ManifestLog::Open(root, writer);
    if(!log.ok())
        return log.status();
    if(hooks.manifest_sync)
        (*log)->setSync(hooks.manifest_sync);
    auto store = std::unique_ptr<FileTierStore>(new FileTierStore(std::move(root),
                                                                  std::move(writer),
                                                                  *std::move(log),
                                                                  std::move(anchors),
                                                                  std::move(codec),
                                                                  std::move(unlink),
                                                                  std::move(load_file),
                                                                  *threads,
                                                                  std::move(decode_file)));
    store->hooks_ = std::move(hooks);
    if(!chain.deployment_id.empty())
    {
        auto status = store->configureTiers(std::move(chain.deployment_id),
                                            std::move(chain.tiers),
                                            chain.io_threads,
                                            chain.io_timeout);
        if(!status.ok())
            return status;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto on_disk = store->recover();
    if(!on_disk.ok())
        return on_disk.status();
    store->queueCommittedCleanup(*on_disk);
    // A durable Deleted record is terminal even when its unlink failed or the process stopped before the unlink.
    // Failures remain pending for the deletion worker and do not prevent serving from the recovered manifest.
    const auto cleanup = store->retryDeletedFiles();
    if(!cleanup.ok())
        LOG_EVERY_N_SEC(ERROR, 10) << "archive Deleted file cleanup failed: " << cleanup;
    LOG(INFO)
            << "archive recovered records=" << store->log_->current()->records.size() << " in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
            << " ms";
    return store;
}

absl::StatusOr<std::unique_ptr<FileTierStore>>
FileTierStore::OpenReadOnly(std::filesystem::path root,
                            std::chrono::milliseconds manifest_poll,
                            LoadFile load_file,
                            size_t read_threads,
                            DecodeFile decode_file,
                            std::chrono::milliseconds archive_read_timeout,
                            TierChain chain,
                            Hooks hooks)
{
    if(!chain.tiers.empty() &&
       (chain.deployment_id.empty() || !chain.io_threads || chain.io_threads > 8 || chain.io_timeout.count() <= 0))
        return absl::InvalidArgumentError("invalid read-only tier configuration");
    if(archive_read_timeout.count() <= 0)
        return absl::InvalidArgumentError("archive read timeout must be positive");
    if(manifest_poll.count() <= 0)
        return absl::InvalidArgumentError("manifest poll must be positive");
    const auto threads = ReaderThreads(read_threads);
    if(!threads.ok())
        return threads.status();
    if(!load_file)
        load_file = LoadChunkFile;
    if(!decode_file)
        decode_file = DecodeChunkFile;
    auto log = ManifestLog::OpenReadOnly(root);
    auto store = std::unique_ptr<FileTierStore>(new FileTierStore(std::move(root),
                                                                  "",
                                                                  std::move(log),
                                                                  {},
                                                                  std::make_shared<ProtoChunkCodec>(),
                                                                  {},
                                                                  std::move(load_file),
                                                                  *threads,
                                                                  std::move(decode_file)));
    store->hooks_ = std::move(hooks);
    if(!chain.tiers.empty())
    {
        const auto& local = chain.tiers.front();
        if(local.name != "local" || local.rank != 0 || local.root.lexically_normal() != store->root_.lexically_normal())
            return absl::InvalidArgumentError("read-only local tier must equal archive root");
        auto tier = std::make_shared<PosixTier>(local, chain.deployment_id, chain.io_threads, chain.io_timeout);
        if(auto status = tier->probe(); !status.ok())
            return status;
        chain.tiers.erase(chain.tiers.begin());
    }
    if(!chain.deployment_id.empty())
    {
        auto status = store->configureTiers(std::move(chain.deployment_id),
                                            std::move(chain.tiers),
                                            chain.io_threads,
                                            chain.io_timeout);
        if(!status.ok())
            return status;
    }
    else if(!chain.tiers.empty())
        return absl::InvalidArgumentError("tier table requires deployment_id");
    store->read_only_ = true;
    store->manifest_poll_ = manifest_poll;
    store->archive_read_timeout_ = archive_read_timeout;
    auto status = store->refreshNow();
    if(!status.ok())
        return status;
    LOG(INFO) << "archive index loaded from manifest records=" << store->log_->current()->records.size();
    return store;
}

absl::Status FileTierStore::refreshNow() const
{
    std::lock_guard lock(mutex_);
    auto index = log_->sync();
    if(!index.ok())
        return index.status();
    polled_ = true;
    refreshed_ = std::chrono::steady_clock::now();
    return absl::OkStatus();
}

absl::Status FileTierStore::registerStory(StoryId story, std::optional<Hlc> anchor)
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    if(!story)
        return absl::InvalidArgumentError("zero story id");
    std::lock_guard lock(mutex_);
    if(anchors_.contains(story) && anchors_.at(story) != anchor)
        return absl::FailedPreconditionError("story anchor already configured");
    anchors_[story] = anchor;
    return absl::OkStatus();
}

bool FileTierStore::known(const ManifestIndex& index, StoryId story) const
{
    return anchors_.contains(story) || index.by_story.contains(story);
}

FileTierStore::StoryView& FileTierStore::viewOf(const ManifestIndex& index, StoryId story) const
{
    auto& view = views_[story];
    const auto found = index.by_story.find(story);
    const size_t count = found == index.by_story.end() ? 0 : found->second.size();
    const auto revisions = index.revisions.find(story);
    const uint64_t revision = revisions == index.revisions.end() ? 0 : revisions->second;
    if(view.built && view.generation == index.generation && view.applied == count && view.revision == revision)
        return view;
    view = StoryView{};
    view.built = true;
    view.generation = index.generation;
    view.applied = count;
    view.revision = revision;
    if(found == index.by_story.end())
        return view;
    std::map<std::string, ManifestRecord> files;
    for(const auto position: found->second)
    {
        const auto& record = index.records[position];
        // Supersession is keyed by the file alone and outranks every record of any writer that names it; a rolled
        // back switch supersedes nothing and its output is in no view (I13.12).
        if(index.rolled_back.contains(record.file))
            continue;
        if(const auto replaced = index.superseded.find(record.file);
           replaced != index.superseded.end() && !index.rolled_back.contains(replaced->second))
        {
            view.superseded[Stem(record.file)] = replaced->second;
            continue;
        }
        const auto key = record.file.empty() ? record.manifest_writer + ":" + record.chunk_id + ":" +
                                                       std::to_string(record.start.physical_ns) + ":" +
                                                       std::to_string(record.start.logical)
                                             : record.file;
        const auto existing = files.find(key);
        if(existing == files.end() || StateRank(record.state) >= StateRank(existing->second.state))
            files[key] = record;
        if(!view.first_start || record.start < *view.first_start)
            view.first_start = record.start;
        if(record.state == ManifestState::Published)
            view.published.insert(record.file);
    }
    for(auto& [key, record]: files) view.effective.push_back(std::move(record));
    view.by_start.resize(view.effective.size());
    for(size_t i = 0; i < view.by_start.size(); ++i) view.by_start[i] = i;
    std::sort(view.by_start.begin(),
              view.by_start.end(),
              [&](size_t a, size_t b) { return view.effective[a].start < view.effective[b].start; });
    return view;
}

std::vector<ManifestRecord> FileTierStore::effective(const ManifestIndex& index, StoryId story) const
{
    return viewOf(index, story).effective;
}

Hlc FileTierStore::watermark(const ManifestIndex& index, StoryId story) const
{
    auto& view = viewOf(index, story);
    std::optional<Hlc> anchor;
    const auto configured = anchors_.find(story);
    if(configured != anchors_.end())
        anchor = configured->second;
    if(!anchor)
        anchor = view.first_start;
    Hlc value = anchor.value_or(Hlc{});
    if(const auto floor = index.watermarks.find(story); floor != index.watermarks.end())
        value = std::max(value, floor->second);
    if(const auto floor = watermarks_.find(story); floor != watermarks_.end())
        value = std::max(value, floor->second);
    if(view.watermark_valid && view.watermark_input == value)
        return view.watermark;
    const Hlc input = value;
    for(const auto position: view.by_start)
    {
        const auto& record = view.effective[position];
        if(record.exempt || (record.state != ManifestState::Published && record.state != ManifestState::Empty &&
                             !(record.state == ManifestState::Deleted && view.published.contains(record.file))))
            continue;
        if(record.start > value)
            break;
        value = std::max(value, record.end);
    }
    watermarks_[story] = value;
    view.watermark_valid = true;
    view.watermark_input = input;
    view.watermark = value;
    return value;
}

absl::StatusOr<const ManifestIndex*> FileTierStore::refresh() const
{
    if(!read_only_)
        return log_->sync();
    auto now = std::chrono::steady_clock::now();
    if(!polled_ || now - refreshed_ >= manifest_poll_)
    {
        auto index = log_->sync();
        if(!index.ok())
            return index.status();
        polled_ = true;
        refreshed_ = now;
        return *index;
    }
    return log_->current();
}

absl::StatusOr<std::vector<Event>> FileTierStore::validate(const ManifestRecord& record,
                                                           std::optional<FileChecksum> checksum) const
{
    auto bytes = loadResolvedFile(record.file);
    if(!bytes.ok())
        return bytes.status();
    if(auto status = VerifyChecksum(*bytes, checksum ? checksum : log_->checksum(record.file)); !status.ok())
        return status;
    auto events = decode_file_(root_ / record.file, *bytes);
    if(!events.ok())
        return events.status();
    if(events->size() != record.event_count)
        return absl::UnavailableError("chunk event count mismatch");
    Chunk chunk{record.chunk_id, record.story_id, record.start, record.end, *std::move(events), record.exempt};
    const auto valid = ValidChunk(chunk);
    if(!valid.ok())
        return absl::UnavailableError(valid.message());
    return std::move(chunk.events);
}

std::optional<ManifestRecord> FileTierStore::successor(const ManifestIndex& index, const ManifestRecord& record) const
{
    const auto replaced = index.superseded.find(record.file);
    if(replaced == index.superseded.end() || index.rolled_back.contains(replaced->second))
        return std::nullopt;
    for(const auto& entry: viewOf(index, record.story_id).effective)
        if(entry.file == replaced->second)
            return entry;
    return std::nullopt;
}

bool FileTierStore::retired(const ManifestIndex& index, const ManifestRecord& record) const
{
    if(const auto next = successor(index, record))
        return next->state == ManifestState::Deleted;
    for(const auto& entry: viewOf(index, record.story_id).effective)
        if(entry.file == record.file)
            return entry.state == ManifestState::Deleted;
    return false;
}

bool FileTierStore::effectivePublished(const ManifestIndex& index, const ManifestRecord& record) const
{
    const auto& view = viewOf(index, record.story_id);
    return std::any_of(view.effective.begin(),
                       view.effective.end(),
                       [&](const auto& entry)
                       { return entry.file == record.file && entry.state == ManifestState::Published; });
}

// A record that vanished after it was planned: retention erased it (no events), compaction replaced it (exactly the
// events the record held, read from the output masked to its window), or it is a real source failure. A successor
// is a compaction output that is never compacted again, so this follows at most one more manifest transition.
absl::StatusOr<std::vector<Event>> FileTierStore::afterVanished(const ManifestRecord& record,
                                                                absl::Status failure,
                                                                Range range,
                                                                size_t max_events,
                                                                uint32_t planned_rank) const
{
    std::string attempted_file = record.file;
    for(int attempt = 0; attempt < 2; ++attempt)
    {
        std::optional<ManifestRecord> next;
        {
            const auto seen = forced_started_.load();
            std::lock_guard lock(mutex_);
            if(forced_done_ <= seen)
            {
                const auto number = ++forced_started_;
                if(!log_->sync().ok())
                    return failure;
                polled_ = true;
                refreshed_ = std::chrono::steady_clock::now();
                forced_done_ = number;
            }
            const auto* index = log_->current();
            // A destroy erases through Deleted records too, and an admitted read that reaches one of them fails (I6.7).
            if(index->tombstoned.contains(record.story_id))
                return failure;
            next = successor(*index, record);
            const auto found = index->locations.find(record.file);
            if(!next && found != index->locations.end() && found->second.rank > planned_rank)
                next = record;
            if(!next)
                return retired(*index, record) ? absl::StatusOr<std::vector<Event>>(std::vector<Event>{}) : failure;
            if(next->state == ManifestState::Deleted)
                return std::vector<Event>{};
            if(next->state != ManifestState::Published)
                return failure;
            const auto moved = index->locations.find(next->file);
            const auto rank = moved == index->locations.end() ? 0 : moved->second.rank;
            if(next->file == attempted_file && rank <= planned_rank)
                return failure;
            attempted_file = next->file;
            planned_rank = rank;
        }
        auto bytes = loadForRead(root_ / next->file);
        if(!bytes.ok())
        {
            failure = bytes.status();
            continue;
        }
        auto events = decodeRecord(*next, range, SIZE_MAX, *std::move(bytes));
        if(!events.ok())
            return events.status();
        std::vector<Event> selected;
        for(auto& event: *events)
        {
            if(event.hlc < record.start || event.hlc >= record.end)
                continue;
            if(selected.size() == max_events)
                break;
            selected.push_back(std::move(event));
        }
        return selected;
    }
    return failure;
}

// Returns every story file found on disk, so committed compaction cleanup only queues files that still exist.
absl::StatusOr<std::set<std::string>> FileTierStore::recover()
{
    auto index = refresh();
    if(!index.ok())
        return index.status();
    std::set<StoryId> stories;
    std::set<std::string> recorded, failed;
    for(const auto& record: (*index)->records)
    {
        stories.insert(record.story_id);
        recorded.insert(record.file);
    }
    for(const auto& [input, output]: (*index)->superseded) recorded.insert(input);
    for(const auto story: stories)
    {
        const auto w = watermark(**index, story);
        for(auto record: effective(**index, story))
        {
            if(record.state != ManifestState::Published &&
               !(record.state == ManifestState::Empty && log_->checksum(record.file)))
                continue;
            if((*index)->locations.contains(record.file))
                continue;
            auto events = validate(record);
            if(events.ok())
                continue;
            if(ArchiveFileVanished(events.status()))
            {
                // A peer may have retired or compacted the file after this index was read: never mark it Lost then.
                auto refreshed = log_->sync();
                if(!refreshed.ok())
                    return refreshed.status();
                if((*refreshed)->locations.contains(record.file))
                    continue;
                const auto current = effective(**refreshed, record.story_id);
                if(!std::any_of(current.begin(),
                                current.end(),
                                [&](const auto& entry)
                                { return entry.file == record.file && entry.state == record.state; }))
                    continue;
            }
            failed.insert(record.file);
            if(record.manifest_writer != writer_)
                continue;
            auto status = rollbackOrLose(record, w);
            if(!status.ok())
                return status;
        }
    }
    std::set<std::string> on_disk;
    std::vector<std::pair<std::filesystem::path, CompactionOutput>> outputs;
    const auto temporary = CompactionTemporaryPrefix(writer_);
    std::error_code error;
    for(std::filesystem::directory_iterator it(root_, error), end; !error && it != end; it.increment(error))
    {
        StoryId story = 0;
        if(!Number(it->path().filename().string(), story) || !story || !it->is_directory(error))
            continue;
        for(std::filesystem::directory_iterator file(it->path(), error), finish; !error && file != finish;
            file.increment(error))
        {
            const auto relative = file->path().lexically_relative(root_);
            on_disk.insert(relative.generic_string());
            if(relative.filename().string().starts_with(temporary))
            {
                // An own compaction temporary is never named by the manifest.
                ::unlink(file->path().c_str());
                continue;
            }
            if(auto output = ParseCompactionOutput(relative))
            {
                if(output->writer == writer_ && !recorded.contains(relative.generic_string()))
                    outputs.emplace_back(relative, *std::move(output));
                continue;
            }
            if(recorded.contains(relative.generic_string()))
                continue;
            auto record = FromFilename(relative);
            if(!record.ok())
                continue;
            tier_detail::Fd owner(record->manifest_writer == writer_
                                          ? -1
                                          : ::open((root_ / "manifest" / (record->manifest_writer + ".log")).c_str(),
                                                   O_RDWR | O_CLOEXEC));
            if(owner.get() >= 0 && ::flock(owner.get(), LOCK_EX | LOCK_NB) != 0)
                continue;
            auto events = validate(*record);
            if(!events.ok())
                continue;
            auto synced = tier_detail::SyncDirectory(it->path());
            if(!synced.ok())
                return synced;
            record->manifest_writer = writer_;
            const auto status = log_->append(*record, BoundsOf(*events));
            if(!status.ok())
                return status;
        }
    }
    if(error)
        return absl::UnavailableError(error.message());
    // An own output no switch names is an aborted compaction. It is removed only while every own effective record in
    // its window is a Published file that just validated and together they cover the window exactly; otherwise it
    // may hold the only copy of those events, so it stays and is reported.
    index = refresh();
    if(!index.ok())
        return index.status();
    for(const auto& [relative, output]: outputs)
    {
        StoryId story = 0;
        (void)Number(relative.parent_path().string(), story);
        auto covered = output.start;
        bool removable = !(*index)->tombstoned.contains(story);
        const auto& view = viewOf(**index, story);
        for(const auto position: view.by_start)
        {
            const auto& record = view.effective[position];
            if(record.manifest_writer != writer_ || record.end <= output.start || record.start >= output.end)
                continue;
            if(record.state != ManifestState::Published || failed.contains(record.file) || record.start != covered)
                removable = false;
            covered = record.end;
        }
        if(!removable || covered != output.end)
        {
            LOG(WARNING) << "archive keeps unreferenced compaction output " << relative.generic_string()
                         << ": its inputs are not all present and valid";
            continue;
        }
        if(::unlink((root_ / relative).c_str()) != 0 && errno != ENOENT)
            return tier_detail::IoError("unlink aborted compaction output");
        on_disk.erase(relative.generic_string());
        auto synced = tier_detail::SyncDirectory((root_ / relative).parent_path());
        if(!synced.ok())
            return synced;
    }
    return on_disk;
}

// An own committed output that fails validation while every input it replaced is still present and valid is rolled
// back rather than lost: the rollback line restores the inputs. Anything else becomes Lost with W preserved (I13.5).
absl::Status FileTierStore::rollbackOrLose(ManifestRecord record, Hlc w)
{
    const auto* index = log_->current();
    const auto change = index->switches.find(record.file);
    if(change != index->switches.end() && change->second.writer == writer_ && !log_->failed())
    {
        const auto inputs = change->second.inputs;
        const bool intact =
                std::all_of(inputs.begin(), inputs.end(), [this](const auto& input) { return validate(input).ok(); });
        if(intact)
        {
            LOG(WARNING) << "archive rolls back compaction output " << record.file << ": its inputs are intact";
            auto status = log_->rememberWatermark(record.story_id, w);
            if(status.ok())
                status = log_->appendRollback(record.story_id, record.file);
            if(status.ok())
                status = refresh().status();
            return status;
        }
    }
    auto status = log_->rememberWatermark(record.story_id, w);
    if(!status.ok())
        return status;
    record.state = ManifestState::Lost;
    return log_->append(record);
}

// Superseded inputs are unlinked only once this incarnation has fsynced its own log, so a switch line that is
// visible but was never durable cannot cost the inputs. Peers' switches are never cleaned here (I13.12).
void FileTierStore::queueCommittedCleanup(const std::set<std::string>& on_disk)
{
    const auto synced = log_->syncOwn();
    std::lock_guard lock(mutex_);
    if(!synced.ok())
    {
        compaction_stopped_ = true;
        LOG(ERROR) << "archive manifest fsync failed at open; compaction and superseded cleanup stop: " << synced;
        return;
    }
    const auto* index = log_->current();
    for(const auto& [output, change]: index->switches)
    {
        if(change.writer != writer_)
            continue;
        if(index->rolled_back.contains(output))
        {
            if(on_disk.contains(output))
                pending_unlinks_[output] = change.story_id;
            continue;
        }
        for(const auto& input: change.inputs)
            if(on_disk.contains(input.file))
                pending_unlinks_[input.file] = change.story_id;
    }
}

absl::StatusOr<ManifestRecord> FileTierStore::publish(Chunk chunk)
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    const auto valid = ValidChunk(chunk);
    if(!valid.ok())
        return valid;
    std::sort(chunk.events.begin(), chunk.events.end(), ReplayLess);
    const auto physical_bounds = BoundsOf(chunk.events);
    const auto name = Filename(chunk, writer_, codec_->extension());
    if(name.size() > 255)
        return absl::InvalidArgumentError("chunk filename too long");
    ManifestRecord record{chunk.id,
                          writer_,
                          (std::filesystem::path(std::to_string(chunk.story_id)) / name).generic_string(),
                          chunk.story_id,
                          chunk.start,
                          chunk.end,
                          chunk.events.size(),
                          chunk.events.empty() ? ManifestState::Empty : ManifestState::Published,
                          chunk.exempt,
                          chunk.physical_policy};
    struct PublishTurn
    {
        FileTierStore& store;
        explicit PublishTurn(FileTierStore& owner)
            : store(owner)
        {
            std::lock_guard lock(store.admission_);
            ++store.publishing_;
        }
        ~PublishTurn()
        {
            {
                std::lock_guard lock(store.admission_);
                --store.publishing_;
            }
            store.admission_changed_.notify_all();
        }
    };
    const auto stem = Stem(record.file);
    // A file already holding this window: the same publication, or the compaction output that superseded it, whose
    // events are compared over the original window only.
    std::optional<ManifestRecord> holder;
    bool compacted = false;
    {
        // The lock covers the manifest view and the claim on the file name, never the file write or a decode, so
        // watermark reports and other stories are not held up by HDF5 or an fsync.
        std::unique_lock lock(mutex_);
        while(true)
        {
            auto index = refresh();
            if(!index.ok())
                return index.status();
            if((*index)->tombstoned.contains(chunk.story_id))
                return absl::FailedPreconditionError("story tombstoned");
            if(!known(**index, chunk.story_id))
                return absl::NotFoundError("unknown story");
            const auto& view = viewOf(**index, chunk.story_id);
            holder.reset();
            compacted = false;
            for(const auto& existing: view.effective)
            {
                if(Stem(existing.file) != stem)
                    continue;
                if(existing.state == ManifestState::Empty && chunk.events.empty())
                {
                    if(!log_->checksum(existing.file))
                        return existing;
                    holder = existing;
                    break;
                }
                if(existing.state != ManifestState::Published)
                    return absl::UnavailableError("chunk rotation already published");
                holder = existing;
                break;
            }
            if(const auto replaced = view.superseded.find(stem); !holder && replaced != view.superseded.end())
            {
                for(const auto& entry: view.effective)
                    if(entry.file == replaced->second && entry.state == ManifestState::Published)
                        holder = entry;
                if(!holder)
                    return absl::UnavailableError("chunk rotation was compacted into a file that is not published");
                compacted = true;
            }
            if(!inflight_.contains(record.file))
                break;
            inflight_changed_.wait(lock);
        }
        inflight_.insert(record.file);
    }
    struct Claim
    {
        FileTierStore& store;
        std::string file;
        ~Claim()
        {
            {
                std::lock_guard lock(store.mutex_);
                store.inflight_.erase(file);
            }
            store.inflight_changed_.notify_all();
        }
    } claim{*this, record.file};
    if(holder)
    {
        // Decoded outside the store mutex: a compaction write can hold the HDF5 mutex for a whole output (I13.12).
        absl::StatusOr<std::vector<Event>> events;
        {
            PublishTurn turn(*this);
            events = validate(*holder);
        }
        if(!events.ok())
            return events.status();
        if(compacted)
            std::erase_if(*events,
                          [&](const auto& event) { return event.hlc < chunk.start || event.hlc >= chunk.end; });
        std::stable_sort(events->begin(), events->end(), ReplayLess);
        const bool same = events->size() == chunk.events.size() &&
                          std::equal(events->begin(), events->end(), chunk.events.begin(), SameEvent);
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(!index.ok())
            return index.status();
        // The answer must name a file that is still the effective holder, never one about to be unlinked.
        const auto& view = viewOf(**index, chunk.story_id);
        const auto replaced = view.superseded.find(stem);
        const bool current =
                (holder->state == ManifestState::Empty
                         ? std::any_of(view.effective.begin(),
                                       view.effective.end(),
                                       [&](const auto& entry)
                                       { return entry.file == holder->file && entry.state == ManifestState::Empty; })
                         : effectivePublished(**index, *holder)) &&
                (compacted ? replaced != view.superseded.end() && replaced->second == holder->file
                           : replaced == view.superseded.end());
        if(!current)
            return absl::UnavailableError("archive file changed during the duplicate check; retry");
        if(!same)
            return absl::UnavailableError("chunk rotation already published");
        return *holder;
    }
    // The holder path above writes no file, so a duplicate transfer settles at the hard stop (I13.16).
    if(auto stop = hardStop(); !stop.ok())
        return stop;
    const auto directory = root_ / std::to_string(chunk.story_id);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if(error)
        return absl::UnavailableError(error.message());
    auto synced = tier_detail::SyncDirectory(root_);
    if(!synced.ok())
        return synced;
    std::string temporary = (directory / ".partial.XXXXXX").string();
    tier_detail::Fd fd(::mkstemp(temporary.data()));
    if(fd.get() < 0)
        return tier_detail::IoError("create chunk temporary");
    struct Cleanup
    {
        const std::string& file;
        ~Cleanup() { ::unlink(file.c_str()); }
    } cleanup{temporary};
    absl::Status status;
    {
        PublishTurn turn(*this);
        status = codec_->writeChunk(temporary, chunk);
    }
    if(!status.ok())
        return status;
    if(::fsync(fd.get()) != 0)
        return tier_detail::IoError("fsync chunk");
    auto checksum = FileChecksumOf(fd.get());
    if(!checksum.ok())
        return checksum.status();
    if(::link(temporary.c_str(), (root_ / record.file).c_str()) != 0)
        return tier_detail::IoError("link published chunk");
    if(::unlink(temporary.c_str()) != 0)
        return tier_detail::IoError("unlink chunk temporary");
    synced = tier_detail::SyncDirectory(directory);
    if(!synced.ok())
        return synced;
    std::lock_guard lock(mutex_);
    status = log_->append(record, physical_bounds, *checksum);
    if(!status.ok())
        return status;
    if(auto index = refresh(); index.ok())
        (void)watermark(**index, chunk.story_id);
    return record;
}

void FileTierStore::setHardStopReserve(uint64_t bytes, FreeBytes free_bytes)
{
    std::lock_guard lock(reserve_mutex_);
    hard_stop_reserve_ = bytes;
    free_bytes_ = std::move(free_bytes);
}

uint64_t FileTierStore::hardStopReserve() const
{
    std::lock_guard lock(reserve_mutex_);
    return hard_stop_reserve_;
}

absl::Status FileTierStore::hardStop() const
{
    uint64_t reserve = 0;
    FreeBytes free_bytes;
    {
        std::lock_guard lock(reserve_mutex_);
        reserve = hard_stop_reserve_;
        free_bytes = free_bytes_;
    }
    if(reserve == 0)
        return absl::OkStatus();
    absl::StatusOr<uint64_t> free = uint64_t{0};
    if(free_bytes)
        free = free_bytes();
    else
    {
        struct statvfs fs
        {
        };
        if(::statvfs(root_.c_str(), &fs) != 0)
            return tier_detail::IoError("statvfs archive root");
        free = static_cast<uint64_t>(fs.f_bavail) * fs.f_frsize;
    }
    if(!free.ok())
        return free.status();
    if(*free < reserve)
        return absl::ResourceExhaustedError("local tier at its hard stop reserve: " + std::to_string(*free) +
                                            " bytes free, hard_stop_reserve_bytes " + std::to_string(reserve));
    return absl::OkStatus();
}

absl::StatusOr<uint64_t> FileTierStore::hardStopReserveBound() const
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    // One migrate_v1 line names a writer, a file, a tier, a uuid, a token and a checksum inside a framed line.
    constexpr uint64_t kMigrationLineBytes = 512;
    uint64_t manifest = 0, own = 0;
    {
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(!index.ok())
            return index.status();
        for(const auto& record: (*index)->records)
            own += record.manifest_writer == writer_ &&
                   (record.state == ManifestState::Published || record.state == ManifestState::Empty);
        for(const auto& path: {log_->logPath(), log_->snapshotPath()})
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if(!error)
                manifest += size;
        }
    }
    return 2 * manifest + own * kMigrationLineBytes;
}

absl::StatusOr<std::vector<Event>> FileTierStore::read(StoryId story, Range range) const
{
    const auto valid = ValidRange(range);
    if(!valid.ok())
        return valid;
    std::vector<ManifestRecord> selected;
    {
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(!index.ok())
            return index.status();
        if(!known(**index, story))
            return absl::NotFoundError("unknown story");
        for(const auto& record: effective(**index, story))
        {
            if(record.state == ManifestState::Published &&
               (range.axis == Range::Axis::Physical ? MayIntersect(**index, record, range)
                                                    : (record.end > range.start && record.start < range.end)))
                selected.push_back(record);
        }
    }
    std::vector<Event> result;
    absl::Status failure;
    for(size_t first = 0; first < selected.size(); first += read_threads_)
    {
        const auto records =
                std::span<const ManifestRecord>(selected).subspan(first,
                                                                  std::min(read_threads_, selected.size() - first));
        for(auto& events: readRecords(records, range))
        {
            if(!events.ok())
                failure.Update(events.status());
            else if(failure.ok())
                result.insert(result.end(),
                              std::make_move_iterator(events->begin()),
                              std::make_move_iterator(events->end()));
        }
    }
    if(!failure.ok())
        return failure;
    std::stable_sort(result.begin(), result.end(), ReplayLess);
    std::set<EventId> seen;
    std::erase_if(result, [&seen](const auto& event) { return !seen.insert(event.id).second; });
    return result;
}

std::vector<absl::StatusOr<std::vector<Event>>>
FileTierStore::readRecords(std::span<const ManifestRecord> records, Range range, size_t max_events) const
{
    using Result = absl::StatusOr<std::vector<Event>>;
    std::vector<Result> results(records.size());
    std::vector<uint32_t> planned_ranks(records.size());
    if(records.empty())
        return results;
    ArchiveReaderPool* readers;
    {
        std::lock_guard lock(mutex_);
        try
        {
            if(!readers_)
                readers_ = std::make_unique<ArchiveReaderPool>(read_threads_);
        }
        catch(const std::exception& error)
        {
            for(auto& result: results) result = absl::ResourceExhaustedError(error.what());
            return results;
        }
        readers = readers_.get();
    }
    using Loaded = absl::StatusOr<ChunkBytes>;
    struct Pending
    {
        size_t index;
        std::future<Loaded> ready;
        ArchiveReaderPool::Deadline deadline;
        std::shared_ptr<PosixTier> tier;
    };
    std::deque<Pending> pending;
    size_t next = 0;
    auto fill = [&]
    {
        while(next < records.size() && pending.size() < read_threads_)
        {
            const size_t index = next++;
            auto readable = canReadRecord(records[index], range);
            if(!readable.ok())
                results[index] = readable.status();
            else if(!*readable)
                results[index] = std::vector<Event>{};
            else
            {
                if(auto migration = log_->location(records[index].file))
                {
                    planned_ranks[index] = migration->rank;
                    std::shared_ptr<PosixTier> tier;
                    {
                        std::lock_guard lock(tier_table_mutex_);
                        auto found = tiers_.find(migration->tier);
                        if(found != tiers_.end() && found->second->config.rank == migration->rank &&
                           found->second->config.tier_uuid == migration->tier_uuid)
                            tier = found->second;
                    }
                    auto directory = tier ? tier->directory() : nullptr;
                    if(!directory)
                    {
                        results[index] = absl::UnavailableError("archive effective tier unavailable");
                        continue;
                    }
                    const auto deadline = std::chrono::steady_clock::now() + tier->timeout();
                    auto ready = tier->submit(
                            [tier, directory, file = records[index].file, hook = hooks_.tier_step]() -> Loaded
                            {
                                if(hook)
                                    if(auto status = hook("read"); !status.ok())
                                        return status;
                                return LoadTierBytes(tier, directory, file);
                            });
                    pending.push_back({index, std::move(ready), deadline, tier});
                    continue;
                }
                const auto deadline = std::chrono::steady_clock::now() + archive_read_timeout_;
                auto task = std::make_shared<std::packaged_task<Loaded()>>(
                        [load = load_file_, file = root_ / records[index].file] { return load(file); });
                auto ready = task->get_future();
                if(readers->submit([task] { (*task)(); }, deadline))
                    pending.push_back({index, std::move(ready), deadline, {}});
                else
                    results[index] = absl::UnavailableError("archive readers unavailable");
            }
        }
    };
    fill();
    while(!pending.empty())
    {
        auto [index, ready, deadline, tier] = std::move(pending.front());
        pending.pop_front();
        try
        {
            if(ready.wait_until(deadline) != std::future_status::ready)
            {
                if(tier)
                    tier->expire();
                else
                    readers->expire();
                results[index] = absl::UnavailableError("archive read deadline exceeded");
                fill();
                continue;
            }
            auto bytes = ready.get();
            results[index] = bytes.ok() ? decodeRecord(records[index], range, max_events, *std::move(bytes))
                                        : Result(bytes.status());
        }
        catch(const std::exception& error)
        {
            results[index] = absl::UnavailableError(error.what());
        }
        fill();
    }
    for(size_t index = 0; index < records.size(); ++index)
        if(!results[index].ok())
            results[index] =
                    afterVanished(records[index], results[index].status(), range, max_events, planned_ranks[index]);
    return results;
}

absl::StatusOr<ChunkBytes> FileTierStore::loadForRead(const std::filesystem::path& file) const
{
    const auto relative = file.lexically_relative(root_).generic_string();
    if(log_->location(relative))
        return loadResolvedFile(relative);
    ArchiveReaderPool* readers;
    {
        std::lock_guard lock(mutex_);
        if(!readers_)
            readers_ = std::make_unique<ArchiveReaderPool>(read_threads_);
        readers = readers_.get();
    }
    const auto deadline = std::chrono::steady_clock::now() + archive_read_timeout_;
    auto task = std::make_shared<std::packaged_task<absl::StatusOr<ChunkBytes>()>>([load = load_file_, file]
                                                                                   { return load(file); });
    auto ready = task->get_future();
    if(!readers->submit([task] { (*task)(); }, deadline))
        return absl::UnavailableError("archive readers unavailable");
    if(ready.wait_until(deadline) != std::future_status::ready)
    {
        readers->expire();
        return absl::UnavailableError("archive read deadline exceeded");
    }
    return ready.get();
}

absl::StatusOr<bool> FileTierStore::canReadRecord(const ManifestRecord& record, Range range) const
{
    const auto valid = ValidRange(range);
    if(!valid.ok())
        return valid;
    if(record.state != ManifestState::Published)
        return absl::InvalidArgumentError("record is not published");
    if(range.axis == Range::Axis::Physical)
    {
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(!index.ok())
            return index.status();
        return MayIntersect(**index, record, range);
    }
    return true;
}

absl::StatusOr<std::vector<Event>>
FileTierStore::readRecord(const ManifestRecord& record, Range range, size_t max_events) const
{
    return readRecords(std::span(&record, 1), range, max_events).front();
}

absl::StatusOr<std::vector<Event>>
FileTierStore::decodeRecord(const ManifestRecord& record, Range range, size_t max_events, ChunkBytes bytes) const
{
    if(auto status = VerifyChecksum(bytes, log_->checksum(record.file)); !status.ok())
        return status;
    auto events = decode_file_(root_ / record.file, bytes);
    if(!events.ok())
        return events.status();
    if(events->size() != record.event_count)
        return absl::UnavailableError("chunk event count mismatch");
    for(const auto& event: *events)
        if(event.id.story_id != record.story_id || event.hlc < record.start || event.hlc >= record.end)
            return absl::UnavailableError("invalid archived event window");
    std::stable_sort(events->begin(), events->end(), ReplayLess);
    std::vector<Event> selected;
    for(auto& event: *events)
    {
        bool matches;
        if(range.axis == Range::Axis::Hlc)
            matches = event.hlc >= range.start && event.hlc < range.end;
        else
        {
            const bool bounded = Bounded(event.physical);
            const __int128_t p = event.physical.physical_ns;
            const __int128_t u = bounded ? *event.physical.uncertainty_ns : 0;
            matches = bounded ? p - u < range.end.physical_ns && p + u >= range.start.physical_ns
                              : p >= range.start.physical_ns && p < range.end.physical_ns;
        }
        if(matches)
        {
            if(selected.size() == max_events)
                break;
            selected.push_back(std::move(event));
        }
    }
    return selected;
}

absl::StatusOr<std::vector<ManifestRecord>> FileTierStore::manifest(StoryId story) const
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    if(!known(**index, story))
        return absl::NotFoundError("unknown story");
    return effective(**index, story);
}

absl::StatusOr<Hlc> FileTierStore::contiguousWatermark(StoryId story) const
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    if(!known(**index, story))
        return absl::NotFoundError("unknown story");
    return watermark(**index, story);
}

absl::StatusOr<bool> FileTierStore::incomplete(StoryId story, Range range) const
{
    const auto valid = ValidRange(range);
    if(!valid.ok())
        return valid;
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    if(!known(**index, story))
        return absl::NotFoundError("unknown story");
    for(const auto& record: effective(**index, story))
        if(record.state == ManifestState::Lost &&
           (range.axis == Range::Axis::Physical || (record.start < range.end && record.end > range.start)))
            return true;
    return false;
}

absl::Status FileTierStore::eraseFile(const std::string& file)
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    std::unique_lock lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    const auto found = std::find_if((*index)->records.begin(),
                                    (*index)->records.end(),
                                    [&file](const auto& record)
                                    {
                                        return record.file == file && (record.state == ManifestState::Published ||
                                                                       record.state == ManifestState::Empty ||
                                                                       record.state == ManifestState::Deleted);
                                    });
    if(found == (*index)->records.end())
        return absl::NotFoundError("unknown archive file");
    // A caller that selected an input before its compaction must reselect from the effective view; erasing the
    // whole output for one old input, or reporting the input erased while the output holds its events, is wrong.
    if(const auto replaced = (*index)->superseded.find(file);
       replaced != (*index)->superseded.end() && !(*index)->rolled_back.contains(replaced->second))
        return absl::FailedPreconditionError("archive file was superseded by compaction; reselect");
    auto record = *found;
    const bool deleted = std::any_of((*index)->records.begin(),
                                     (*index)->records.end(),
                                     [&file](const auto& entry)
                                     { return entry.file == file && entry.state == ManifestState::Deleted; });
    if(!deleted)
    {
        auto status = log_->rememberWatermark(record.story_id, watermark(**index, record.story_id));
        if(!status.ok())
            return status;
        record.state = ManifestState::Deleted;
        status = log_->append(record);
        if(!status.ok())
            return status;
    }
    pending_unlinks_[file] = record.story_id;
    index = refresh();
    if(!index.ok())
        return index.status();
    collectDeletedFiles(**index);
    // The durable deletion takes effect now; its physical cleanup waits for the existing job's claim.
    if(auto active = claims_.find(file); active != claims_.end() && !active->second.expired())
        return absl::OkStatus();
    lock.unlock();
    const auto status = unlinkDeletedFile(file);
    if(status.ok())
    {
        lock.lock();
        pending_unlinks_.erase(file);
    }
    return status;
}

void FileTierStore::collectDeletedFiles(const ManifestIndex& index)
{
    if(deletion_generation_ != index.generation || deletion_applied_ > index.records.size())
    {
        deletion_generation_ = index.generation;
        deletion_applied_ = 0;
    }
    while(deletion_applied_ < index.records.size())
    {
        const auto& record = index.records[deletion_applied_++];
        if(record.state == ManifestState::Deleted && !record.file.empty())
            pending_unlinks_[record.file] = record.story_id;
    }
}

absl::Status FileTierStore::unlinkDeletedFile(const std::string& file)
{
    std::shared_ptr<Claim> held;
    std::vector<std::shared_ptr<PosixTier>> targets;
    {
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(!index.ok())
            return index.status();
        auto pending = pending_unlinks_.find(file);
        if(pending == pending_unlinks_.end())
            return absl::AbortedError("unlink no longer pending");
        const auto story = pending->second;
        bool eligible = (*index)->tombstoned.contains(story);
        for(const auto& entry: (*index)->records)
            if(entry.file == file && entry.state == ManifestState::Deleted)
                eligible = true;
        if(auto it = (*index)->superseded.find(file);
           it != (*index)->superseded.end() && !(*index)->rolled_back.contains(it->second))
            eligible = true;
        if((*index)->rolled_back.contains(file))
            eligible = true;
        if(!eligible)
            return absl::AbortedError("unlink no longer Deleted or superseded");
        if(!(*index)->tombstoned.contains(story) && ArchiveFileWriter(file) != std::optional<std::string>(writer_))
            return absl::FailedPreconditionError("cannot unlink another writer's file");
        for(const auto& [name, tier]: tiers_) targets.push_back(tier);
        auto work = tier_unlinks_.find(file);
        if(work != tier_unlinks_.end())
        {
            if(std::any_of(unlink_results_.at(file)->begin(),
                           unlink_results_.at(file)->end(),
                           [](auto& future)
                           { return future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready; }))
            {
                if(std::chrono::steady_clock::now() >= tier_unlink_deadlines_.at(file))
                    for(const auto& tier: targets) tier->expire();
                return absl::UnavailableError("tier unlink pending");
            }
            auto status = work->second.get();
            tier_unlinks_.erase(work);
            unlink_results_.erase(file);
            tier_unlink_deadlines_.erase(file);
            if(!status.ok())
                return status;
            targets.clear();
        }
        if(claims_.contains("sweep/" + std::to_string(story)) &&
           !claims_.at("sweep/" + std::to_string(story)).expired())
            return absl::UnavailableError("story tier sweep already claimed");
        held = claim(file, story);
        if(!held)
            return absl::UnavailableError("archive file already claimed");
        if(!targets.empty())
        {
            std::vector<std::pair<std::shared_ptr<PosixTier>, std::shared_ptr<TierDirectory>>> roots;
            for(const auto& tier: targets)
            {
                auto directory = tier->directory();
                if(!directory)
                    return absl::UnavailableError("tier deletion pending availability");
                roots.emplace_back(tier, directory);
            }
            // Each tier's work owns its claim and descriptor; the caller only collects ready results.
            auto futures = std::make_shared<std::vector<std::future<absl::Status>>>();
            for(const auto& [tier, directory]: roots)
                futures->push_back(tier->submit(
                        [tier, directory, held, file, hook = hooks_.tier_step]
                        {
                            if(hook)
                            {
                                auto status = hook("erase");
                                if(!status.ok())
                                    return status;
                            }
                            auto status = tier->verify(*directory);
                            if(status.ok())
                                status = PosixTier::erase(directory->fd.get(), file);
                            if(status.ok())
                                status = tier->verify(*directory);
                            return status;
                        }));
            tier_unlinks_[file] = std::async(std::launch::deferred,
                                             [futures]() mutable
                                             {
                                                 absl::Status status;
                                                 for(auto& future: *futures)
                                                 {
                                                     try
                                                     {
                                                         status.Update(future.get());
                                                     }
                                                     catch(const std::exception& error)
                                                     {
                                                         status.Update(absl::UnavailableError(error.what()));
                                                     }
                                                 }
                                                 return status;
                                             });
            // Deferred collection does not execute I/O and is drained only once every task is ready.
            unlink_results_[file] = futures;
            auto timeout = targets.front()->timeout();
            for(const auto& tier: targets) timeout = std::min(timeout, tier->timeout());
            tier_unlink_deadlines_[file] = std::chrono::steady_clock::now() + timeout;
            return absl::UnavailableError("tier unlink queued");
        }
    }
    if(unlink_(root_ / file) != 0)
    {
        if(errno == ENOENT)
            return absl::OkStatus();
        const auto status = tier_detail::IoError("delete archived file");
        LOG_EVERY_N_SEC(ERROR, 10) << "cannot unlink Deleted archive file " << file << ": " << status;
        return status;
    }
    const auto status = tier_detail::SyncDirectory((root_ / file).parent_path());
    if(!status.ok())
        LOG_EVERY_N_SEC(ERROR, 10) << "cannot sync Deleted archive file directory " << file << ": " << status;
    return status;
}

// In a tombstoned story any writer may unlink any writer's superseded inputs, rolled back or unreferenced
// compaction outputs and compaction temporaries, so a destroy completes while the compacting writer is down for good.
void FileTierStore::sweepTombstoned()
{
    std::vector<StoryId> stories;
    {
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(!index.ok())
            return;
        for(const auto story: (*index)->tombstoned)
            if(!swept_.contains(story))
                stories.push_back(story);
    }
    for(const auto story: stories)
    {
        std::vector<std::string> found;
        std::error_code error;
        for(std::filesystem::directory_iterator it(root_ / std::to_string(story), error), end; !error && it != end;
            it.increment(error))
        {
            const auto relative = it->path().lexically_relative(root_);
            if(relative.filename().string().starts_with(".compact-") || ParseCompactionOutput(relative))
                found.push_back(relative.generic_string());
        }
        if(error && error != std::errc::no_such_file_or_directory)
            continue;
        std::lock_guard lock(mutex_);
        const auto* index = log_->current();
        for(auto record: effective(*index, story))
            if(record.state == ManifestState::Empty)
            {
                auto status = log_->rememberWatermark(story, watermark(*index, story));
                record.state = ManifestState::Deleted;
                if(status.ok())
                    status = log_->append(record);
                if(status.ok())
                    pending_unlinks_[record.file] = story;
            }
        for(const auto& [output, change]: index->switches)
            if(change.story_id == story)
                for(const auto& input: change.inputs)
                    if(!index->rolled_back.contains(output))
                        pending_unlinks_[input.file] = story;
        for(const auto& file: found)
            if(!index->switches.contains(file) || index->rolled_back.contains(file) ||
               file.find("/.compact-") != std::string::npos)
                pending_unlinks_[file] = story;
        swept_.insert(story);
    }
}

absl::Status FileTierStore::retryDeletedFiles()
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    sweepTombstoned();
    queueTierSweeps();
    std::vector<std::string> files;
    {
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(!index.ok())
            return index.status();
        collectDeletedFiles(**index);
        for(const auto& [file, story]: pending_unlinks_) files.push_back(file);
    }
    absl::Status result;
    for(const auto& file: files)
    {
        const auto status = unlinkDeletedFile(file);
        result.Update(status);
        if(status.ok())
        {
            std::lock_guard lock(mutex_);
            pending_unlinks_.erase(file);
        }
    }
    return result;
}

absl::StatusOr<bool> FileTierStore::hasPendingUnlinks(StoryId story)
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    collectDeletedFiles(**index);
    if((*index)->tombstoned.contains(story))
        for(const auto& [name, tier]: tiers_)
            if(!tier_swept_.contains({name, story}))
                return true;
    if(((*index)->tombstoned.contains(story) && !swept_.contains(story)) || compacting_.load() == story)
        return true;
    for(const auto& [file, weak]: claims_)
        if(auto held = weak.lock(); held && held->story == story)
            return true;
    return std::any_of(pending_unlinks_.begin(),
                       pending_unlinks_.end(),
                       [story](const auto& pending) { return pending.second == story; });
}

absl::Status FileTierStore::tombstone(StoryId story)
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    if(!story)
        return absl::InvalidArgumentError("zero story id");
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    if((*index)->tombstoned.contains(story))
        return absl::OkStatus();
    return log_->appendTombstone(story);
}

absl::StatusOr<bool> FileTierStore::tombstoned(StoryId story) const
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    return (*index)->tombstoned.contains(story);
}

absl::StatusOr<std::vector<StoryId>> FileTierStore::tombstonedStories() const
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    return std::vector<StoryId>((*index)->tombstoned.begin(), (*index)->tombstoned.end());
}

absl::StatusOr<std::vector<StoryId>> FileTierStore::liveStories() const
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    std::vector<StoryId> stories;
    for(const auto& [story, positions]: (*index)->by_story)
        if(!(*index)->tombstoned.contains(story))
            stories.push_back(story);
    return stories;
}

absl::StatusOr<std::vector<StoryId>> FileTierStore::storiesWithoutPhysicalPolicy() const
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    return std::vector<StoryId>((*index)->without_physical_policy.begin(), (*index)->without_physical_policy.end());
}

absl::Status FileTierStore::compact()
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    std::lock_guard lock(mutex_);
    return log_->compact();
}
struct FileTierStore::CompactionJob
{
    StoryId story{};
    std::vector<ManifestRecord> inputs;
    std::vector<uint64_t> sizes;
    std::vector<std::shared_ptr<Claim>> claims;
};

absl::Status FileTierStore::compactionStep(std::string_view step) const
{
    return hooks_.compaction_step ? hooks_.compaction_step(step) : absl::OkStatus();
}

void FileTierStore::stopCompaction()
{
    {
        std::lock_guard lock(admission_);
        stop_compaction_ = true;
    }
    admission_changed_.notify_all();
}

// Admits one compaction codec call or file read: never while a publish is in its codec call, and only within the
// byte budget, whose debt the next call pays. Holds no store, manifest or codec lock while it waits.
bool FileTierStore::waitCompactionTurn(uint64_t bytes, const CompactionPolicy& policy)
{
    std::unique_lock lock(admission_);
    while(true)
    {
        if(stop_compaction_)
            return false;
        if(publishing_)
        {
            admission_changed_.wait(lock);
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        const double rate = static_cast<double>(policy.io_bytes_per_sec);
        const double burst = static_cast<double>(std::max(policy.io_burst_bytes, uint64_t{1}));
        if(io_refilled_ == std::chrono::steady_clock::time_point{})
            io_tokens_ = burst;
        else
            io_tokens_ = std::min(burst, io_tokens_ + std::chrono::duration<double>(now - io_refilled_).count() * rate);
        io_refilled_ = now;
        if(io_tokens_ > 0)
        {
            io_tokens_ -= static_cast<double>(bytes);
            return true;
        }
        admission_changed_.wait_for(lock, std::chrono::duration<double>((1.0 - io_tokens_) / rate));
    }
}

absl::StatusOr<CompactionResult> FileTierStore::compactOnce(const CompactionPolicy& policy)
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    if(policy.min_files < 2 || policy.max_files < policy.min_files || !policy.max_events || policy.max_events > 65536 ||
       !policy.max_output_bytes || !policy.io_bytes_per_sec || policy.max_span_ns <= 0)
        return absl::InvalidArgumentError("invalid compaction policy");
    std::vector<std::pair<StoryId, std::vector<ManifestRecord>>> runs;
    std::map<std::string, std::shared_ptr<Claim>> selection_claims;
    {
        std::lock_guard lock(mutex_);
        if(compaction_stopped_ || log_->failed())
        {
            compaction_stopped_ = true;
            return absl::FailedPreconditionError("archive compaction stopped after a manifest failure");
        }
        auto index = refresh();
        if(!index.ok())
            return index.status();
        std::vector<StoryId> stories;
        for(const auto& [story, positions]: (*index)->by_story)
            if(!(*index)->tombstoned.contains(story))
                stories.push_back(story);
        if(stories.empty())
            return CompactionResult{};
        std::rotate(stories.begin(),
                    stories.begin() + static_cast<long>(next_story_++ % stories.size()),
                    stories.end());
        for(const auto story: stories)
        {
            const auto w = watermark(**index, story);
            const auto& view = viewOf(**index, story);
            std::vector<ManifestRecord> run;
            const auto close = [&]
            {
                if(run.size() >= policy.min_files)
                    runs.emplace_back(story, std::move(run));
                run.clear();
            };
            for(const auto position: view.by_start)
            {
                const auto& record = view.effective[position];
                const bool own = record.manifest_writer == writer_;
                const bool claimed = std::any_of(inflight_.begin(),
                                                 inflight_.end(),
                                                 [&](const auto& file) { return Stem(file) == Stem(record.file); });
                const bool eligible = own && record.state == ManifestState::Published && !record.exempt &&
                                      record.end <= w && record.event_count <= policy.max_events && !claimed &&
                                      !(*index)->locations.contains(record.file) &&
                                      (!claims_.contains(record.file) || claims_[record.file].expired()) &&
                                      !ParseCompactionOutput(record.file);
                if(!eligible)
                {
                    // Another writer's copy of a window neither joins nor breaks a run; anything else breaks it.
                    if(own || record.exempt ||
                       (record.state != ManifestState::Published && record.state != ManifestState::Empty))
                        close();
                    continue;
                }
                if(!run.empty() &&
                   (run.back().end != record.start || run.back().physical_policy != record.physical_policy))
                    close();
                selection_claims[record.file] = claim(record.file, story);
                run.push_back(record);
            }
            close();
        }
    }
    const auto now = std::chrono::system_clock::now();
    for(auto& [story, run]: runs)
    {
        CompactionJob job{story, {}, {}, {}};
        uint64_t bytes = 0, events = 0;
        for(const auto& record: run)
        {
            struct stat info
            {
            };
            tier_detail::Fd fd(::open((root_ / record.file).c_str(), O_RDONLY | O_CLOEXEC));
            const bool opened = fd.get() >= 0 && ::fstat(fd.get(), &info) == 0;
            const auto size = opened ? static_cast<uint64_t>(info.st_size) : 0;
            const auto modified = std::chrono::system_clock::time_point(std::chrono::seconds(info.st_mtim.tv_sec) +
                                                                        std::chrono::nanoseconds(info.st_mtim.tv_nsec));
            const bool usable = opened && size <= policy.small_file_bytes && size <= policy.max_output_bytes &&
                                now - modified >= policy.min_age &&
                                record.end.physical_ns - record.start.physical_ns <= policy.max_span_ns;
            const bool fits = !job.inputs.empty() && job.inputs.size() < policy.max_files &&
                              events + record.event_count <= policy.max_events &&
                              bytes + size <= policy.max_output_bytes &&
                              record.end.physical_ns - job.inputs.front().start.physical_ns <= policy.max_span_ns;
            if(!usable || (!job.inputs.empty() && !fits))
            {
                if(job.inputs.size() >= policy.min_files)
                    break;
                job.inputs.clear();
                job.sizes.clear();
                job.claims.clear();
                bytes = events = 0;
                if(!usable)
                    continue;
            }
            job.claims.push_back(selection_claims.at(record.file));
            job.inputs.push_back(record);
            job.sizes.push_back(size);
            bytes += size;
            events += record.event_count;
        }
        if(job.inputs.size() >= policy.min_files)
            return runCompaction(policy, std::move(job));
    }
    return CompactionResult{};
}

absl::StatusOr<CompactionResult> FileTierStore::runCompaction(const CompactionPolicy& policy, CompactionJob job)
{
    compacting_ = job.story;
    struct Active
    {
        std::atomic<StoryId>& story;
        ~Active() { story = 0; }
    } active{compacting_};
    if(auto stop = hardStop(); !stop.ok())
        return stop;
    // Files this job created, removed on every abort before the switch. A step hook that stops the job models a
    // crash and leaves them for recovery.
    struct Created
    {
        std::vector<std::filesystem::path> files;
        bool keep{};
        ~Created()
        {
            if(!keep)
                for(const auto& file: files) ::unlink(file.c_str());
        }
    } created;
    const auto story = job.story;
    const auto& inputs = job.inputs;
    std::vector<Event> events;
    for(size_t i = 0; i < inputs.size(); ++i)
    {
        if(!waitCompactionTurn(job.sizes[i], policy))
            return absl::CancelledError("archive compaction stopped");
        auto loaded = validate(inputs[i]);
        if(!loaded.ok())
            return loaded.status();
        events.insert(events.end(), std::make_move_iterator(loaded->begin()), std::make_move_iterator(loaded->end()));
    }
    std::vector<EventId> ids;
    for(const auto& event: events) ids.push_back(event.id);
    std::sort(ids.begin(), ids.end());
    if(std::adjacent_find(ids.begin(), ids.end()) != ids.end())
        return absl::DataLossError("compaction inputs repeat an event id");
    std::sort(events.begin(), events.end(), ReplayLess);
    const auto op = RandomOp();
    Chunk chunk{op, story, inputs.front().start, inputs.back().end, events, false, inputs.front().physical_policy};
    if(const auto valid = ValidChunk(chunk); !valid.ok())
        return valid;
    const auto directory = root_ / std::to_string(story);
    const auto relative = std::filesystem::path(std::to_string(story)) /
                          CompactionOutputName({writer_, op, chunk.start, chunk.end}, codec_->extension());
    const auto final = root_ / relative;
    std::string temporary = (directory / (CompactionTemporaryPrefix(writer_) + "XXXXXX")).string();
    tier_detail::Fd fd(::mkstemp(temporary.data()));
    if(fd.get() < 0)
        return tier_detail::IoError("create compaction temporary");
    created.files.push_back(temporary);
    uint64_t estimate = 0;
    for(const auto size: job.sizes) estimate += size;
    if(!waitCompactionTurn(estimate, policy))
        return absl::CancelledError("archive compaction stopped");
    if(auto status = codec_->writeChunk(temporary, chunk); !status.ok())
        return status;
    if(::fsync(fd.get()) != 0)
        return tier_detail::IoError("fsync compaction output");
    // The codec writes by path. A tombstone sweep may have unlinked the temporary while this job waited, and the
    // codec then wrote a new file that this fsync and checksum never covered; stop before linking it.
    struct stat opened
    {
    }, named{};
    if(::fstat(fd.get(), &opened) != 0 || ::stat(temporary.c_str(), &named) != 0 || opened.st_ino != named.st_ino ||
       opened.st_dev != named.st_dev)
    {
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(index.ok() && (*index)->tombstoned.contains(story))
            return absl::FailedPreconditionError("story tombstoned during compaction");
        return absl::DataLossError("compaction temporary was replaced before link");
    }
    auto checksum = FileChecksumOf(fd.get());
    if(!checksum.ok())
        return checksum.status();
    if(auto status = compactionStep("link"); !status.ok())
    {
        created.keep = true;
        return status;
    }
    // link fails rather than replaces an existing name, also on NFS (I13.1).
    if(::link(temporary.c_str(), final.c_str()) != 0)
        return tier_detail::IoError("link compaction output");
    created.files.push_back(final);
    if(::unlink(temporary.c_str()) != 0)
        return tier_detail::IoError("unlink compaction temporary");
    if(auto synced = tier_detail::SyncDirectory(directory); !synced.ok())
        return synced;
    ManifestRecord output{op,
                          writer_,
                          relative.generic_string(),
                          story,
                          chunk.start,
                          chunk.end,
                          events.size(),
                          ManifestState::Published,
                          false,
                          chunk.physical_policy};
    if(!waitCompactionTurn(estimate, policy))
        return absl::CancelledError("archive compaction stopped");
    auto written = validate(output, *checksum);
    if(!written.ok())
        return written.status();
    std::stable_sort(written->begin(), written->end(), ReplayLess);
    if(written->size() != events.size() || !std::equal(written->begin(), written->end(), events.begin(), SameEvent))
        return absl::DataLossError("compaction output differs from its inputs");
    if(auto status = compactionStep("switch"); !status.ok())
    {
        created.keep = true;
        return status;
    }
    CompactionSwitch change{writer_, op, story, inputs, output, BoundsOf(events), {}, *checksum};
    {
        std::lock_guard lock(mutex_);
        if(compaction_stopped_ || log_->failed())
        {
            compaction_stopped_ = true;
            return absl::FailedPreconditionError("archive compaction stopped after a manifest failure");
        }
        // The final check reads the manifest again under the mutex that also orders tombstones and erasures, and
        // appends the switch without releasing it.
        auto index = refresh();
        if(!index.ok())
            return index.status();
        if((*index)->tombstoned.contains(story))
            return absl::FailedPreconditionError("story tombstoned during compaction");
        const auto& view = viewOf(**index, story);
        for(const auto& input: inputs)
        {
            const bool same = std::any_of(view.effective.begin(),
                                          view.effective.end(),
                                          [&](const auto& entry)
                                          {
                                              return entry.file == input.file &&
                                                     entry.state == ManifestState::Published &&
                                                     entry.manifest_writer == writer_ && entry.start == input.start &&
                                                     entry.end == input.end && entry.event_count == input.event_count;
                                          });
            const bool claimed = std::any_of(inflight_.begin(),
                                             inflight_.end(),
                                             [&](const auto& file) { return Stem(file) == Stem(input.file); });
            if(!same || claimed || (*index)->locations.contains(input.file) || !claims_.contains(input.file) ||
               claims_[input.file].expired() ||
               std::find(job.claims.begin(), job.claims.end(), claims_[input.file].lock()) == job.claims.end())
                return absl::AbortedError("compaction input changed before the switch");
        }
        change.w_floor = watermark(**index, story);
        if(inputs.back().end > change.w_floor)
            return absl::AbortedError("compaction inputs are above the watermark");
        if(auto status = log_->appendSwitch(change); !status.ok())
        {
            // The line may be complete in the log without being durable: keep both sides and stop compacting.
            compaction_stopped_ = true;
            created.keep = true;
            LOG(ERROR) << "archive compaction switch for " << output.file
                       << " is not durable; compaction stops: " << status;
            return status;
        }
        created.keep = true;
        if(auto installed = refresh(); installed.ok())
            (void)watermark(**installed, story);
    }
    if(auto status = compactionStep("cleanup"); !status.ok())
        return status;
    // Fed only by this thread after its own fsync of the switch succeeded (I13.12).
    for(const auto& input: inputs)
    {
        absl::Status status;
        if(unlink_(root_ / input.file) != 0 && errno != ENOENT)
            status = tier_detail::IoError("unlink compaction input");
        else
            status = tier_detail::SyncDirectory((root_ / input.file).parent_path());
        if(!status.ok())
        {
            std::lock_guard lock(mutex_);
            pending_unlinks_[input.file] = story;
        }
    }
    LOG(INFO) << "archive compacted story=" << story << " inputs=" << inputs.size() << " events=" << events.size()
              << " output=" << output.file;
    return CompactionResult{inputs.size(), output.file};
}

std::shared_ptr<FileTierStore::Claim> FileTierStore::claim(const std::string& file, StoryId story)
{
    if(!claims_[file].expired())
        return {};
    auto result = std::make_shared<Claim>(Claim{story, RandomOp()});
    claims_[file] = result;
    return result;
}

absl::Status FileTierStore::configureTiers(std::string deployment,
                                           std::vector<TierConfig> tiers,
                                           size_t threads,
                                           std::chrono::milliseconds timeout)
{
    if(deployment.empty() || !threads || threads > 8 || timeout.count() <= 0)
        return absl::InvalidArgumentError("invalid tier executor configuration");
    std::map<std::string, std::shared_ptr<PosixTier>> configured;
    std::set<uint32_t> ranks;
    for(auto& tier: tiers)
    {
        if(tier.name.empty() || tier.name == "local" || tier.kind != "posix" || !tier.rank || tier.tier_uuid.empty() ||
           tier.root.empty() || !ranks.insert(tier.rank).second || configured.contains(tier.name))
            return absl::InvalidArgumentError("invalid tier table");
        const auto name = tier.name;
        configured[name] = std::make_shared<PosixTier>(std::move(tier), deployment, threads, timeout);
    }
    std::lock_guard lock(mutex_);
    if(!tiers_.empty())
        return absl::FailedPreconditionError("tier table already configured");
    deployment_ = std::move(deployment);
    std::lock_guard tier_lock(tier_table_mutex_);
    tiers_ = std::move(configured);
    return absl::OkStatus();
}

absl::Status FileTierStore::probeTiers(std::chrono::milliseconds timeout) const
{
    std::vector<std::shared_ptr<PosixTier>> tiers;
    {
        std::lock_guard lock(mutex_);
        for(const auto& [name, tier]: tiers_) tiers.push_back(tier);
    }
    absl::Status result;
    for(const auto& tier: tiers) result.Update(tier->probe(timeout));
    return result;
}

absl::StatusOr<std::optional<MigrationLocation>> FileTierStore::location(const std::string& file) const
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return index.status();
    auto it = (*index)->locations.find(file);
    if(it == (*index)->locations.end())
        return std::optional<MigrationLocation>{};
    return std::optional<MigrationLocation>{it->second};
}

bool FileTierStore::migrationEligible(const ManifestIndex& index,
                                      const ManifestRecord& record,
                                      const std::shared_ptr<Claim>& own) const
{
    std::ifstream mark(root_ / "manifest" / (writer_ + ".validated"));
    try
    {
        nlohmann::json validated;
        mark >> validated;
        if(validated.at("writer").get<std::string>() != writer_ || !validated.at("through").is_number_unsigned())
            return false;
        auto seq = index.record_sequences.find(record.file);
        if(seq != index.record_sequences.end() && seq->second > validated.at("through").get<uint64_t>())
            return false;
    }
    catch(const std::exception&)
    {
        return false;
    }
    if(record.manifest_writer != writer_ || ArchiveFileWriter(record.file) != std::optional<std::string>(writer_) ||
       index.tombstoned.contains(record.story_id) || record.end > watermark(index, record.story_id))
        return false;
    if(auto it = claims_.find(record.file); it != claims_.end())
        if(auto held = it->second.lock(); held && held != own)
            return false;
    if(auto it = index.superseded.find(record.file);
       it != index.superseded.end() && !index.rolled_back.contains(it->second))
        return false;
    for(const auto& file: inflight_)
        if(Stem(file) == Stem(record.file))
            return false;
    for(const auto& entry: effective(index, record.story_id))
        if(entry.file == record.file)
        {
            if(entry.state != ManifestState::Published && entry.state != ManifestState::Empty)
                return false;
            if(auto change = index.switches.find(record.file); change != index.switches.end())
                for(const auto& input: change->second.inputs)
                    if(pending_unlinks_.contains(input.file) ||
                       (claims_.contains(input.file) && !claims_.at(input.file).expired()))
                        return false;
            return true;
        }
    return false;
}

absl::StatusOr<size_t> FileTierStore::migrateOnce(const std::string& destination)
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
    ManifestRecord record;
    MigrationLocation migration;
    std::shared_ptr<Claim> held;
    std::shared_ptr<PosixTier> tier, source_tier;
    std::shared_ptr<TierDirectory> directory, source_directory;
    {
        std::lock_guard lock(mutex_);
        if(migration_stopped_ || log_->failed())
            return absl::FailedPreconditionError("migration stopped after manifest failure");
        auto target = tiers_.find(destination);
        if(target == tiers_.end())
            return absl::InvalidArgumentError("unknown migration tier");
        tier = target->second;
        directory = tier->directory();
        if(!directory)
            return absl::UnavailableError("migration tier unavailable");
        auto index = refresh();
        if(!index.ok())
            return index.status();
        // TIER-1 owns validated marks. Legacy records have sequence zero.
        std::ifstream mark(root_ / "manifest" / (writer_ + ".validated"));
        nlohmann::json validated;
        try
        {
            mark >> validated;
        }
        catch(const std::exception&)
        {
            return size_t{0};
        }
        if(validated.value("writer", std::string{}) != writer_ || !validated.contains("through"))
            return size_t{0};
        for(const auto& [story, positions]: (*index)->by_story)
        {
            for(const auto& candidate: effective(**index, story))
            {
                if(!migrationEligible(**index, candidate))
                    continue;
                auto prior = (*index)->locations.find(candidate.file);
                if(prior != (*index)->locations.end())
                {
                    if(prior->second.rank >= tier->config.rank)
                        continue;
                    auto source = tiers_.find(prior->second.tier);
                    if(source == tiers_.end() || source->second->config.tier_uuid != prior->second.tier_uuid)
                        continue;
                    source_tier = source->second;
                    source_directory = source_tier->directory();
                    if(!source_directory)
                        continue;
                }
                if(prior == (*index)->locations.end())
                {
                    source_tier.reset();
                    source_directory.reset();
                }
                record = candidate;
                held = claim(record.file, story);
                migration = {writer_,
                             record.file,
                             tier->config.name,
                             tier->config.tier_uuid,
                             held->token,
                             story,
                             tier->config.rank,
                             {}};
                break;
            }
            if(held)
                break;
        }
    }
    if(!held)
    {
        auto status = cleanupMigrations();
        if(status.ok())
            status = sweepTiers();
        if(status.ok())
            status = writeTierReplicas();
        if(!status.ok())
            return status;
        return size_t{0};
    }
    const auto step = hooks_.migration_step;
    if(step)
    {
        auto status = step(1);
        if(!status.ok())
            return status;
    }
    absl::StatusOr<std::string> bytes = absl::UnavailableError("source unavailable");
    if(source_tier)
        bytes = source_tier->run(
                [source_tier, source_directory, held, file = record.file]() -> absl::StatusOr<std::string>
                {
                    auto status = source_tier->verify(*source_directory);
                    if(!status.ok())
                        return status;
                    return PosixTier::read(source_directory->fd.get(), file);
                });
    else
    {
        tier_detail::Fd root(::open(root_.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC));
        if(root.get() >= 0)
            bytes = PosixTier::read(root.get(), record.file);
    }
    if(!bytes.ok())
        return bytes.status();
    migration.checksum = {bytes->size(), static_cast<uint32_t>(absl::ComputeCrc32c(*bytes))};
    const auto expected = log_->checksum(record.file);
    if(!expected || expected->bytes != migration.checksum.bytes || expected->crc32c != migration.checksum.crc32c)
        return absl::UnavailableError("migration source checksum mismatch");
    const auto temporary = std::to_string(record.story_id) + "/.migrate-" + Hex(writer_) + "." + held->token;
    auto created = std::make_shared<std::atomic<bool>>(false);
    auto copied = tier->run(
            [tier, directory, held, migration, temporary, bytes = *std::move(bytes), step, created]() -> absl::Status
            {
                const auto cleanup = [&]
                {
                    (void)PosixTier::erase(directory->fd.get(), temporary);
                    if(created->load())
                        (void)PosixTier::erase(directory->fd.get(), migration.file);
                };
                auto status = tier->verify(*directory);
                if(!status.ok())
                    return status;
                const auto story = std::to_string(migration.story_id);
                if(::mkdirat(directory->fd.get(), story.c_str(), 0755) != 0 && errno != EEXIST)
                    return tier_detail::IoError("create tier story");
                tier_detail::Fd fd(::openat(directory->fd.get(),
                                            temporary.c_str(),
                                            O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                                            0644));
                if(fd.get() < 0)
                    return tier_detail::IoError("create migration temporary");
                status = tier_detail::WriteAll(fd.get(), bytes);
                if(status.ok() && ::fsync(fd.get()) != 0)
                    status = tier_detail::IoError("sync migration temporary");
                if(!status.ok())
                {
                    cleanup();
                    return status;
                }
                if(step)
                {
                    status = step(2);
                    if(!status.ok())
                        return status;
                }
                if(::linkat(directory->fd.get(), temporary.c_str(), directory->fd.get(), migration.file.c_str(), 0) !=
                   0)
                {
                    if(errno != EEXIST)
                    {
                        status = tier_detail::IoError("link migration destination");
                        cleanup();
                        return status;
                    }
                    auto old = PosixTier::read(directory->fd.get(), migration.file, true);
                    if(!old.ok() || *old != bytes)
                    {
                        status = PosixTier::erase(directory->fd.get(), migration.file);
                        if(!status.ok())
                        {
                            cleanup();
                            return status;
                        }
                        if(::linkat(directory->fd.get(),
                                    temporary.c_str(),
                                    directory->fd.get(),
                                    migration.file.c_str(),
                                    0) != 0)
                        {
                            status = tier_detail::IoError("replace migration destination");
                            cleanup();
                            return status;
                        }
                        *created = true;
                    }
                }
                else
                    *created = true;
                status = PosixTier::erase(directory->fd.get(), temporary);
                if(!status.ok())
                {
                    cleanup();
                    return status;
                }
                if(step)
                {
                    status = step(3);
                    if(!status.ok())
                        return status;
                }
                auto verified = PosixTier::read(directory->fd.get(), migration.file, true);
                if(!verified.ok() || *verified != bytes)
                {
                    cleanup();
                    return absl::UnavailableError("migration verification failed");
                }
                if(step)
                {
                    status = step(4);
                    if(!status.ok())
                        return status;
                }
                status = tier->verify(*directory);
                if(!tier->current(directory) || !status.ok())
                {
                    cleanup();
                    return absl::UnavailableError("migration availability epoch changed");
                }
                return absl::OkStatus();
            });
    if(!copied.ok())
        return copied;
    if(step)
    {
        auto status = step(5);
        if(!status.ok())
            return status;
    }
    // Re-read the marker on the executor, immediately before acquiring the commit mutex.
    auto verified = tier->run([tier, directory, held] { return tier->verify(*directory); });
    absl::Status committed = verified;
    bool keep = false;
    {
        std::lock_guard lock(mutex_);
        if(committed.ok())
        {
            auto index = refresh();
            if(!index.ok())
                committed = index.status();
            else if(migration_stopped_ || log_->failed() || !tier->current(directory) ||
                    claims_[record.file].lock() != held || !migrationEligible(**index, record, held))
                committed = absl::AbortedError("migration eligibility changed before commit");
            else
            {
                committed = log_->syncOwn();
                if(committed.ok())
                    committed = log_->appendMigration(migration);
                if(!committed.ok())
                {
                    migration_stopped_ = true;
                    stopped_migrations_.push_back(held);
                    keep = true;
                }
                else
                {
                    keep = true;
                    committed = refresh().status();
                }
            }
        }
    }
    if(!keep)
    {
        (void)tier->run(
                [tier, directory, held, temporary, created, file = record.file]
                {
                    auto status = PosixTier::erase(directory->fd.get(), temporary);
                    if(created->load())
                        status.Update(PosixTier::erase(directory->fd.get(), file));
                    return status;
                });
    }
    if(!committed.ok())
        return committed;
    if(step)
    {
        auto status = step(6);
        if(!status.ok())
            return status;
    }
    absl::Status removed;
    if(source_tier)
        removed = source_tier->run(
                [source_tier, source_directory, held, file = record.file]
                {
                    auto status = source_tier->verify(*source_directory);
                    if(status.ok())
                        status = PosixTier::erase(source_directory->fd.get(), file);
                    if(status.ok())
                        status = source_tier->verify(*source_directory);
                    return status;
                });
    else
    {
        if(unlink_(root_ / record.file) != 0 && errno != ENOENT)
            removed = tier_detail::IoError("unlink migration source");
        else
            removed = tier_detail::SyncDirectory((root_ / record.file).parent_path());
    }
    if(!removed.ok())
        return removed;
    auto replica = writeTierReplicas();
    if(!replica.ok())
        return replica;
    return size_t{1};
}

absl::Status FileTierStore::sweepTiers()
{
    std::vector<std::shared_ptr<PosixTier>> tiers;
    {
        std::lock_guard lock(mutex_);
        for(const auto& [name, tier]: tiers_) tiers.push_back(tier);
    }
    absl::Status result;
    for(const auto& tier: tiers)
    {
        auto directory = tier->directory();
        if(!directory)
        {
            result.Update(absl::UnavailableError("tier sweep pending availability"));
            continue;
        }
        auto names = tier->run(
                [tier, directory]() -> absl::StatusOr<std::vector<std::string>>
                {
                    auto status = tier->verify(*directory);
                    if(!status.ok())
                        return status;
                    auto stories = PosixTier::list(directory->fd.get(), ".");
                    if(!stories.ok())
                        return stories.status();
                    std::vector<std::string> names;
                    for(const auto& story: *stories)
                    {
                        StoryId id;
                        if(!Number(story, id) || !id)
                            continue;
                        auto files = PosixTier::list(directory->fd.get(), story);
                        if(!files.ok())
                            return files.status();
                        names.insert(names.end(), files->begin(), files->end());
                    }
                    status = tier->verify(*directory);
                    if(!status.ok())
                        return status;
                    return names;
                });
        if(!names.ok())
        {
            result.Update(names.status());
            continue;
        }
        for(const auto& file: *names)
        {
            std::shared_ptr<Claim> held;
            {
                std::lock_guard lock(mutex_);
                auto index = refresh();
                if(!index.ok())
                {
                    result.Update(index.status());
                    break;
                }
                StoryId story;
                if(!Number(std::filesystem::path(file).parent_path().string(), story))
                    continue;
                const bool destroyed = (*index)->tombstoned.contains(story);
                if(destroyed)
                {
                    bool live = false;
                    for(const auto& record: effective(**index, story))
                        if(record.state == ManifestState::Published || record.state == ManifestState::Empty)
                            live = true;
                    if(live)
                        continue;
                }
                else
                {
                    if(ArchiveFileWriter(file) != std::optional<std::string>(writer_) &&
                       !std::filesystem::path(file).filename().string().starts_with(".migrate-" + Hex(writer_) + "."))
                        continue;
                    if((*index)->locations.contains(file))
                        continue;
                    bool active_temporary = false;
                    for(const auto& [name, weak]: claims_)
                        if(auto active = weak.lock(); active && file.ends_with("." + active->token))
                            active_temporary = true;
                    if(active_temporary)
                        continue;
                }
                held = claim(file, story);
                if(!held)
                    continue;
            }
            result.Update(tier->run(
                    [tier, directory, held, file, hook = hooks_.tier_step]
                    {
                        if(hook)
                        {
                            auto status = hook("sweep");
                            if(!status.ok())
                                return status;
                        }
                        auto status = tier->verify(*directory);
                        if(status.ok())
                            status = PosixTier::erase(directory->fd.get(), file);
                        if(status.ok())
                            status = tier->verify(*directory);
                        return status;
                    }));
        }
    }
    return result;
}

absl::Status FileTierStore::cleanupMigrations()
{
    std::vector<MigrationLocation> migrations;
    {
        std::lock_guard lock(mutex_);
        auto index = refresh();
        if(!index.ok())
            return index.status();
        auto status = log_->syncOwn();
        if(!status.ok())
            return status;
        for(const auto& [file, location]: (*index)->locations)
            if(location.writer == writer_)
                migrations.push_back(location);
    }
    absl::Status result;
    for(const auto& migration: migrations)
    {
        std::shared_ptr<Claim> held;
        std::shared_ptr<PosixTier> target;
        std::vector<std::shared_ptr<PosixTier>> faster;
        {
            std::lock_guard lock(mutex_);
            auto index = refresh();
            if(!index.ok())
            {
                result.Update(index.status());
                continue;
            }
            if((*index)->tombstoned.contains(migration.story_id) || !tiers_.contains(migration.tier))
                continue;
            held = claim(migration.file, migration.story_id);
            if(!held)
                continue;
            target = tiers_.at(migration.tier);
            for(const auto& [name, tier]: tiers_)
                if(tier->config.rank < migration.rank)
                    faster.push_back(tier);
        }
        auto directory = target->directory();
        if(!directory || target->config.tier_uuid != migration.tier_uuid)
            continue;
        const auto verify = [target, directory, held, migration]
        {
            auto status = target->verify(*directory);
            if(!status.ok())
                return status;
            auto bytes = PosixTier::read(directory->fd.get(), migration.file, true);
            if(!bytes.ok())
                return bytes.status();
            if(bytes->size() != migration.checksum.bytes ||
               static_cast<uint32_t>(absl::ComputeCrc32c(*bytes)) != migration.checksum.crc32c)
                return absl::UnavailableError("migration cleanup destination checksum mismatch");
            return target->verify(*directory);
        };
        auto verified = target->run(verify);
        if(!verified.ok() && target->current(directory))
            verified = target->run(verify);
        if(!verified.ok() && target->current(directory))
        {
            // An intact faster copy repairs the destination without changing the effective location.
            absl::StatusOr<std::string> source = absl::UnavailableError("no stale migration source");
            tier_detail::Fd local(::open(root_.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC));
            if(local.get() >= 0)
                source = PosixTier::read(local.get(), migration.file);
            const auto intact = [&](const absl::StatusOr<std::string>& bytes)
            {
                return bytes.ok() && bytes->size() == migration.checksum.bytes &&
                       static_cast<uint32_t>(absl::ComputeCrc32c(*bytes)) == migration.checksum.crc32c;
            };
            if(!intact(source))
                for(const auto& tier: faster)
                {
                    auto root = tier->directory();
                    if(!root)
                        continue;
                    source = tier->run(
                            [tier, root, held, migration]() -> absl::StatusOr<std::string>
                            {
                                auto status = tier->verify(*root);
                                if(!status.ok())
                                    return status;
                                return PosixTier::read(root->fd.get(), migration.file);
                            });
                    if(intact(source))
                        break;
                }
            if(intact(source))
                verified = target->run(
                        [target, directory, held, migration, bytes = *std::move(source), writer = writer_]
                        {
                            auto status = target->verify(*directory);
                            if(!status.ok())
                                return status;
                            const auto story = std::to_string(migration.story_id);
                            if(::mkdirat(directory->fd.get(), story.c_str(), 0755) != 0 && errno != EEXIST)
                                return tier_detail::IoError("create migration repair story");
                            const auto temporary = story + "/.migrate-" + Hex(writer) + "." + held->token;
                            tier_detail::Fd fd(::openat(directory->fd.get(),
                                                        temporary.c_str(),
                                                        O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                                                        0644));
                            if(fd.get() < 0)
                                return tier_detail::IoError("create migration repair temporary");
                            status = tier_detail::WriteAll(fd.get(), bytes);
                            if(status.ok() && ::fsync(fd.get()) != 0)
                                status = tier_detail::IoError("sync migration repair");
                            if(status.ok() && !target->current(directory))
                                status = absl::UnavailableError("migration repair abandoned");
                            if(status.ok())
                                status = PosixTier::erase(directory->fd.get(), migration.file);
                            bool created = false;
                            if(status.ok())
                            {
                                if(::linkat(directory->fd.get(),
                                            temporary.c_str(),
                                            directory->fd.get(),
                                            migration.file.c_str(),
                                            0) != 0)
                                    status = tier_detail::IoError("install migration repair");
                                else
                                    created = true;
                            }
                            status.Update(PosixTier::erase(directory->fd.get(), temporary));
                            if(status.ok())
                            {
                                auto read = PosixTier::read(directory->fd.get(), migration.file, true);
                                if(!read.ok() || *read != bytes)
                                    status = absl::UnavailableError("migration repair verification failed");
                            }
                            if(status.ok())
                                status = target->verify(*directory);
                            if(!target->current(directory) && created)
                            {
                                (void)PosixTier::erase(directory->fd.get(), migration.file);
                                return absl::UnavailableError("migration repair abandoned");
                            }
                            return status;
                        });
        }
        if(!verified.ok())
        {
            result.Update(verified);
            continue;
        }
        if(unlink_(root_ / migration.file) != 0 && errno != ENOENT)
            result.Update(tier_detail::IoError("unlink stale migration source"));
        else if(std::filesystem::exists((root_ / migration.file).parent_path()))
            result.Update(tier_detail::SyncDirectory((root_ / migration.file).parent_path()));
        for(const auto& tier: faster)
        {
            auto root = tier->directory();
            if(!root)
            {
                result.Update(absl::UnavailableError("stale migration tier unavailable"));
                continue;
            }
            result.Update(tier->run(
                    [tier, root, held, file = migration.file]
                    {
                        auto status = tier->verify(*root);
                        if(status.ok())
                            status = PosixTier::erase(root->fd.get(), file);
                        if(status.ok())
                            status = tier->verify(*root);
                        return status;
                    }));
        }
    }
    return result;
}

absl::Status FileTierStore::writeTierReplicas()
{
    std::string bytes;
    std::vector<std::shared_ptr<PosixTier>> tiers;
    {
        std::lock_guard lock(mutex_);
        auto status = log_->compact();
        if(!status.ok())
            return status;
        std::ifstream snapshot(log_->snapshotPath(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(snapshot), {});
        if(snapshot.bad())
            return absl::UnavailableError("read manifest replica source");
        for(const auto& [name, tier]: tiers_) tiers.push_back(tier);
    }
    absl::Status result;
    for(const auto& tier: tiers)
    {
        auto directory = tier->directory();
        if(!directory)
            continue;
        result.Update(tier->run(
                [tier, directory, bytes, writer = writer_, token = RandomOp()]
                {
                    auto status = tier->verify(*directory);
                    if(!status.ok())
                        return status;
                    if(::mkdirat(directory->fd.get(), "manifest-replica", 0755) != 0 && errno != EEXIST)
                        return tier_detail::IoError("create manifest replica directory");
                    const auto temporary = "manifest-replica/." + writer + "." + token;
                    const auto final = "manifest-replica/" + writer + ".snap";
                    tier_detail::Fd fd(::openat(directory->fd.get(),
                                                temporary.c_str(),
                                                O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                                                0644));
                    if(fd.get() < 0)
                        return tier_detail::IoError("create manifest replica");
                    status = tier_detail::WriteAll(fd.get(), bytes);
                    if(status.ok() && ::fsync(fd.get()) != 0)
                        status = tier_detail::IoError("sync manifest replica");
                    if(status.ok() &&
                       ::renameat(directory->fd.get(), temporary.c_str(), directory->fd.get(), final.c_str()) != 0)
                        status = tier_detail::IoError("install manifest replica");
                    if(!status.ok())
                    {
                        (void)::unlinkat(directory->fd.get(), temporary.c_str(), 0);
                        return status;
                    }
                    tier_detail::Fd parent(::openat(directory->fd.get(),
                                                    "manifest-replica",
                                                    O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
                    if(parent.get() < 0 || ::fsync(parent.get()) != 0)
                        return tier_detail::IoError("sync replica directory");
                    return tier->verify(*directory);
                }));
    }
    return result;
}
absl::Status FileTierStore::awaitTierUnlinksForTesting(std::chrono::milliseconds timeout)
{
    std::vector<std::shared_ptr<std::vector<std::future<absl::Status>>>> work;
    {
        std::lock_guard lock(mutex_);
        for(const auto& [file, futures]: unlink_results_) work.push_back(futures);
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for(const auto& futures: work)
        for(auto& future: *futures)
            if(future.wait_until(deadline) != std::future_status::ready)
                return absl::UnavailableError("test tier unlink deadline expired");
    return retryDeletedFiles();
}

absl::StatusOr<ChunkBytes> FileTierStore::loadResolvedFile(const std::string& file) const
{
    auto migration = log_->location(file);
    if(!migration)
        return load_file_(root_ / file);
    std::shared_ptr<PosixTier> tier;
    {
        std::lock_guard lock(tier_table_mutex_);
        auto found = tiers_.find(migration->tier);
        if(found == tiers_.end() || found->second->config.rank != migration->rank ||
           found->second->config.tier_uuid != migration->tier_uuid)
            return absl::UnavailableError("archive effective tier refused");
        tier = found->second;
    }
    auto directory = tier->directory();
    if(!directory)
        return absl::UnavailableError("archive effective tier unavailable");
    return tier->run([tier, directory, file] { return LoadTierBytes(tier, directory, file); });
}

void FileTierStore::queueTierSweeps()
{
    std::lock_guard lock(mutex_);
    auto index = refresh();
    if(!index.ok())
        return;
    for(auto it = tier_sweeps_.begin(); it != tier_sweeps_.end();)
    {
        if(it->second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        {
            ++it;
            continue;
        }
        if(it->second.get().ok())
            tier_swept_.insert(it->first);
        it = tier_sweeps_.erase(it);
    }
    for(const auto story: (*index)->tombstoned)
    {
        if(!swept_.contains(story))
            continue;
        bool live = false;
        for(const auto& record: effective(**index, story))
            if(record.manifest_writer == writer_ &&
               (record.state == ManifestState::Published || record.state == ManifestState::Empty))
                live = true;
        if(live)
            continue;
        bool claimed = false;
        for(const auto& [file, weak]: claims_)
            if(auto held = weak.lock(); held && held->story == story)
                claimed = true;
        if(claimed)
            continue;
        auto held = claim("sweep/" + std::to_string(story), story);
        for(const auto& [name, tier]: tiers_)
        {
            const auto key = std::make_pair(name, story);
            if(tier_swept_.contains(key) || tier_sweeps_.contains(key))
                continue;
            auto directory = tier->directory();
            if(!directory)
                continue;
            tier_sweeps_[key] = tier->submit(
                    [tier, directory, held, story, hook = hooks_.tier_step]
                    {
                        if(hook)
                        {
                            auto status = hook("sweep");
                            if(!status.ok())
                                return status;
                        }
                        auto status = tier->verify(*directory);
                        if(!status.ok())
                            return status;
                        auto files = PosixTier::list(directory->fd.get(), std::to_string(story));
                        if(!files.ok())
                            return files.status();
                        for(const auto& file: *files)
                        {
                            status = PosixTier::erase(directory->fd.get(), file);
                            if(!status.ok())
                                return status;
                        }
                        return tier->verify(*directory);
                    });
        }
    }
}

} // namespace chronolog
