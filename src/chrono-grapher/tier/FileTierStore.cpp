#include <absl/log/log.h>
#include "chrono-grapher/tier/FileTierStore.h"
#include "chrono-grapher/tier/ArchiveReaderPool.h"
#include "chrono-grapher/tier/FileIO.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <future>
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

FileTierStore::~FileTierStore() = default;

absl::StatusOr<std::unique_ptr<FileTierStore>> FileTierStore::Open(std::filesystem::path root,
                                                                   std::string writer,
                                                                   std::map<StoryId, Hlc> anchors,
                                                                   std::shared_ptr<const ChunkCodec> codec,
                                                                   Unlink unlink,
                                                                   LoadFile load_file,
                                                                   size_t read_threads,
                                                                   DecodeFile decode_file,
                                                                   Hooks hooks)
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

absl::StatusOr<std::unique_ptr<FileTierStore>> FileTierStore::OpenReadOnly(std::filesystem::path root,
                                                                           std::chrono::milliseconds manifest_poll,
                                                                           LoadFile load_file,
                                                                           size_t read_threads,
                                                                           DecodeFile decode_file)
{
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
    store->read_only_ = true;
    store->manifest_poll_ = manifest_poll;
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

absl::StatusOr<std::vector<Event>> FileTierStore::validate(const ManifestRecord& record) const
{
    auto bytes = load_file_(root_ / record.file);
    if(!bytes.ok())
        return bytes.status();
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
absl::StatusOr<std::vector<Event>>
FileTierStore::afterVanished(const ManifestRecord& record, absl::Status failure, Range range, size_t max_events) const
{
    for(int attempt = 0; attempt < 2 && ArchiveFileVanished(failure); ++attempt)
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
            if(!next)
                return retired(*index, record) ? absl::StatusOr<std::vector<Event>>(std::vector<Event>{}) : failure;
            if(next->state == ManifestState::Deleted)
                return std::vector<Event>{};
            if(next->state != ManifestState::Published)
                return failure;
        }
        auto bytes = load_file_(root_ / next->file);
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
            if(record.state != ManifestState::Published)
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
                if(!effectivePublished(**refreshed, record))
                    continue;
            }
            failed.insert(record.file);
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
                    return existing;
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
            events = ReadChunkFile(root_ / holder->file);
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
        const bool current = effectivePublished(**index, *holder) &&
                             (compacted ? replaced != view.superseded.end() && replaced->second == holder->file
                                        : replaced == view.superseded.end());
        if(!current)
            return absl::UnavailableError("archive file changed during the duplicate check; retry");
        if(!same)
            return absl::UnavailableError("chunk rotation already published");
        return *holder;
    }
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
    if(::link(temporary.c_str(), (root_ / record.file).c_str()) != 0)
        return tier_detail::IoError("link published chunk");
    if(::unlink(temporary.c_str()) != 0)
        return tier_detail::IoError("unlink chunk temporary");
    synced = tier_detail::SyncDirectory(directory);
    if(!synced.ok())
        return synced;
    std::lock_guard lock(mutex_);
    status = log_->append(record, physical_bounds);
    if(!status.ok())
        return status;
    if(auto index = refresh(); index.ok())
        (void)watermark(**index, chunk.story_id);
    return record;
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
    std::deque<std::pair<size_t, std::future<Loaded>>> pending;
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
                auto task = std::make_shared<std::packaged_task<Loaded()>>([this, file = root_ / records[index].file]
                                                                           { return load_file_(file); });
                pending.emplace_back(index, task->get_future());
                if(!readers->submit([task] { (*task)(); }))
                    results[index] = absl::UnavailableError("archive readers stopping");
            }
        }
    };
    fill();
    while(!pending.empty())
    {
        auto [index, ready] = std::move(pending.front());
        pending.pop_front();
        try
        {
            auto bytes = ready.get();
            results[index] = bytes.ok() ? decodeRecord(records[index], range, max_events, *std::move(bytes))
                                        : afterVanished(records[index], bytes.status(), range, max_events);
        }
        catch(const std::exception& error)
        {
            results[index] = absl::UnavailableError(error.what());
        }
        fill();
    }
    return results;
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
    auto readable = canReadRecord(record, range);
    if(!readable.ok())
        return readable.status();
    if(!*readable)
        return std::vector<Event>{};
    auto bytes = load_file_(root_ / record.file);
    if(!bytes.ok())
        return afterVanished(record, bytes.status(), range, max_events);
    return decodeRecord(record, range, max_events, *std::move(bytes));
}

absl::StatusOr<std::vector<Event>>
FileTierStore::decodeRecord(const ManifestRecord& record, Range range, size_t max_events, ChunkBytes bytes) const
{
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
    if(((*index)->tombstoned.contains(story) && !swept_.contains(story)) || compacting_.load() == story)
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
                run.push_back(record);
            }
            close();
        }
    }
    const auto now = std::chrono::system_clock::now();
    for(auto& [story, run]: runs)
    {
        CompactionJob job{story, {}, {}};
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
                bytes = events = 0;
                if(!usable)
                    continue;
            }
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
    auto written = validate(output);
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
    CompactionSwitch change{writer_, op, story, inputs, output, BoundsOf(events), {}};
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
            if(!same || claimed)
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
        if(!unlinkDeletedFile(input.file).ok())
        {
            std::lock_guard lock(mutex_);
            pending_unlinks_[input.file] = story;
        }
    LOG(INFO) << "archive compacted story=" << story << " inputs=" << inputs.size() << " events=" << events.size()
              << " output=" << output.file;
    return CompactionResult{inputs.size(), output.file};
}
} // namespace chronolog
