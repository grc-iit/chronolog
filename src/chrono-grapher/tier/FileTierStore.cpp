#include <absl/log/log.h>
#include "chrono-grapher/tier/FileTierStore.h"
#include "chrono-grapher/tier/ArchiveReaderPool.h"
#include "chrono-grapher/tier/FileIO.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <future>
#include <limits>
#include <set>
#include <sstream>
#include <sys/file.h>
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

absl::StatusOr<std::vector<Event>> ValidateFile(const std::filesystem::path& root, const ManifestRecord& record)
{
    auto events = ReadChunkFile(root / record.file);
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
} // namespace

FileTierStore::FileTierStore(std::filesystem::path root,
                             std::string writer,
                             std::unique_ptr<ManifestLog> log,
                             std::map<StoryId, Hlc> anchors,
                             std::shared_ptr<const ChunkCodec> codec,
                             Unlink unlink,
                             ReadFile read_file,
                             size_t read_threads)
    : root_(std::move(root))
    , writer_(std::move(writer))
    , log_(std::move(log))
    , codec_(std::move(codec))
    , unlink_(std::move(unlink))
    , read_file_(std::move(read_file))
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
                                                                   ReadFile read_file,
                                                                   size_t read_threads)
{
    if(!codec || anchors.contains(0))
        return absl::InvalidArgumentError("invalid tier configuration");
    const auto threads = ReaderThreads(read_threads);
    if(!threads.ok())
        return threads.status();
    if(!unlink)
        unlink = [](const std::filesystem::path& path) { return ::unlink(path.c_str()); };
    if(!read_file)
        read_file = ReadChunkFile;
    auto log = ManifestLog::Open(root, writer);
    if(!log.ok())
        return log.status();
    auto store = std::unique_ptr<FileTierStore>(new FileTierStore(std::move(root),
                                                                  std::move(writer),
                                                                  *std::move(log),
                                                                  std::move(anchors),
                                                                  std::move(codec),
                                                                  std::move(unlink),
                                                                  std::move(read_file),
                                                                  *threads));
    const auto started = std::chrono::steady_clock::now();
    const auto status = store->recover();
    if(!status.ok())
        return status;
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
                                                                           ReadFile read_file,
                                                                           size_t read_threads)
{
    if(manifest_poll.count() <= 0)
        return absl::InvalidArgumentError("manifest poll must be positive");
    const auto threads = ReaderThreads(read_threads);
    if(!threads.ok())
        return threads.status();
    if(!read_file)
        read_file = ReadChunkFile;
    auto log = ManifestLog::OpenReadOnly(root);
    auto store = std::unique_ptr<FileTierStore>(new FileTierStore(std::move(root),
                                                                  "",
                                                                  std::move(log),
                                                                  {},
                                                                  std::make_shared<ProtoChunkCodec>(),
                                                                  {},
                                                                  std::move(read_file),
                                                                  *threads));
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
    if(view.built && view.generation == index.generation && view.applied == count)
        return view;
    view = StoryView{};
    view.built = true;
    view.generation = index.generation;
    view.applied = count;
    if(found == index.by_story.end())
        return view;
    std::map<std::string, ManifestRecord> files;
    for(const auto position: found->second)
    {
        const auto& record = index.records[position];
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

absl::Status FileTierStore::recover()
{
    auto index = refresh();
    if(!index.ok())
        return index.status();
    std::set<StoryId> stories;
    std::set<std::string> recorded;
    for(const auto& record: (*index)->records)
    {
        stories.insert(record.story_id);
        recorded.insert(record.file);
    }
    for(const auto story: stories)
    {
        const auto w = watermark(**index, story);
        for(auto record: effective(**index, story))
        {
            if(record.state != ManifestState::Published)
                continue;
            if(ValidateFile(root_, record).ok())
                continue;
            auto status = log_->rememberWatermark(story, w);
            if(!status.ok())
                return status;
            record.state = ManifestState::Lost;
            status = log_->append(record);
            if(!status.ok())
                return status;
        }
    }
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
            auto events = ValidateFile(root_, *record);
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
    return absl::OkStatus();
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
    {
        // The lock covers the manifest view and the claim on the file name, never the file write, so watermark
        // reports and other stories are not held up by an HDF5 write or an fsync.
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
            for(const auto& existing: viewOf(**index, chunk.story_id).effective)
            {
                auto existing_name = std::filesystem::path(existing.file);
                auto requested_name = std::filesystem::path(record.file);
                if(existing_name.replace_extension() != requested_name.replace_extension())
                    continue;
                if(existing.state == ManifestState::Empty && chunk.events.empty())
                    return existing;
                if(existing.state == ManifestState::Published)
                {
                    auto events = ReadChunkFile(root_ / existing.file);
                    if(!events.ok())
                        return events.status();
                    if(events->size() == chunk.events.size() &&
                       std::equal(events->begin(), events->end(), chunk.events.begin(), SameEvent))
                        return existing;
                }
                return absl::UnavailableError("chunk rotation already published");
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
    auto status = codec_->writeChunk(temporary, chunk);
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
    ArchiveReaderPool* readers = nullptr;
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
        if(selected.empty())
            return std::vector<Event>{};
        if(!readers_)
        {
            try
            {
                readers_ = std::make_unique<ArchiveReaderPool>(read_threads_);
            }
            catch(const std::exception& error)
            {
                return absl::ResourceExhaustedError(error.what());
            }
        }
        readers = readers_.get();
    }
    using Result = absl::StatusOr<std::vector<Event>>;
    std::vector<std::future<Result>> pending;
    pending.reserve(selected.size());
    for(auto& record: selected)
    {
        auto task = std::make_shared<std::packaged_task<Result()>>([this, record = std::move(record), range]
                                                                   { return readFileRecord(record, range); });
        pending.push_back(task->get_future());
        if(!readers->submit([task] { (*task)(); }))
            return absl::UnavailableError("archive readers stopping");
    }
    std::vector<Event> result;
    absl::Status failure;
    for(auto& ready: pending)
    {
        Result events;
        try
        {
            events = ready.get();
        }
        catch(const std::exception& error)
        {
            events = absl::UnavailableError(error.what());
        }
        if(!events.ok())
            failure.Update(events.status());
        else if(failure.ok())
            result.insert(result.end(),
                          std::make_move_iterator(events->begin()),
                          std::make_move_iterator(events->end()));
    }
    if(!failure.ok())
        return failure;
    std::stable_sort(result.begin(), result.end(), ReplayLess);
    std::set<EventId> seen;
    std::erase_if(result, [&seen](const auto& event) { return !seen.insert(event.id).second; });
    return result;
}

absl::StatusOr<std::vector<Event>>
FileTierStore::readRecord(const ManifestRecord& record, Range range, size_t max_events) const
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
        if(!MayIntersect(**index, record, range))
            return std::vector<Event>{};
    }
    return readFileRecord(record, range, max_events);
}

absl::StatusOr<std::vector<Event>>
FileTierStore::readFileRecord(const ManifestRecord& record, Range range, size_t max_events) const
{
    const auto valid = ValidRange(range);
    if(!valid.ok())
        return valid;
    if(record.state != ManifestState::Published)
        return absl::InvalidArgumentError("record is not published");
    auto events = read_file_(root_ / record.file);
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

absl::Status FileTierStore::retryDeletedFiles()
{
    if(read_only_)
        return absl::FailedPreconditionError("read-only tier store");
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
} // namespace chronolog
