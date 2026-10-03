#include "chrono-grapher/tier/ManifestLog.h"
#include "chrono-grapher/tier/FileIO.h"
#include <absl/log/check.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>

namespace chronolog
{
namespace
{
using Json = nlohmann::json;

bool SafeWriter(const std::string& writer)
{
    return !writer.empty() && writer.size() <= 64 &&
           std::all_of(writer.begin(),
                       writer.end(),
                       [](unsigned char c) {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                  c == '-' || c == '_';
                       });
}

Json Encode(const ManifestRecord& record, std::optional<PhysicalBounds> bounds)
{
    Json json = {{"chunk", record.chunk_id},
                 {"writer", record.manifest_writer},
                 {"file", record.file},
                 {"story", record.story_id},
                 {"start", {record.start.physical_ns, record.start.logical}},
                 {"end", {record.end.physical_ns, record.end.logical}},
                 {"count", record.event_count},
                 {"state", static_cast<int>(record.state)},
                 {"exempt", record.exempt},
                 {"physical_policy", record.physical_policy}};
    if(bounds)
        json["physical_bounds"] = {{"min_lo", bounds->min_lo},
                                   {"max_hi", bounds->max_hi},
                                   {"unbounded", bounds->unbounded}};
    return json;
}

std::optional<PhysicalBounds> DecodeBounds(const Json& json, ManifestState state)
{
    if(!json.contains("physical_bounds"))
        return std::nullopt;
    const auto& bounds = json.at("physical_bounds");
    auto integer = [&](const char* key)
    {
        const auto& value = bounds.at(key);
        if(!value.is_number_integer() ||
           (value.is_number_unsigned() &&
            value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())))
            throw std::runtime_error("invalid physical bound integer");
        return value.get<int64_t>();
    };
    PhysicalBounds result{integer("min_lo"), integer("max_hi"), bounds.at("unbounded").get<bool>()};
    if(state != ManifestState::Published || result.min_lo > result.max_hi)
        throw std::runtime_error("invalid archive physical bounds");
    return result;
}

ManifestRecord Decode(const Json& json)
{
    ManifestRecord record;
    record.chunk_id = json.at("chunk").get<std::string>();
    record.manifest_writer = json.at("writer").get<std::string>();
    record.file = json.at("file").get<std::string>();
    record.story_id = json.at("story").get<StoryId>();
    record.start = {json.at("start").at(0).get<int64_t>(), json.at("start").at(1).get<uint32_t>()};
    record.end = {json.at("end").at(0).get<int64_t>(), json.at("end").at(1).get<uint32_t>()};
    record.event_count = json.at("count").get<uint64_t>();
    const auto state = json.at("state").get<int>();
    if(state < 0 || state > static_cast<int>(ManifestState::Lost))
        throw std::runtime_error("invalid manifest state");
    record.state = static_cast<ManifestState>(state);
    record.exempt = json.at("exempt").get<bool>();
    record.physical_policy = json.value("physical_policy", false);
    if(record.start >= record.end || !record.story_id || !SafeWriter(record.manifest_writer))
        throw std::runtime_error("invalid manifest identity");
    const std::filesystem::path file(record.file);
    if(!record.file.empty() && (file.is_absolute() || file.parent_path() != std::to_string(record.story_id)))
        throw std::runtime_error("invalid manifest file path");
    return record;
}

absl::StatusOr<std::vector<std::string>> ReadLines(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if(!input)
    {
        if(errno == ENOENT)
            return absl::NotFoundError("manifest does not exist");
        return absl::UnavailableError("cannot open manifest " + path.string());
    }
    std::vector<std::string> lines;
    std::string line;
    while(std::getline(input, line))
    {
        if(input.eof())
            break;
        if(!line.empty())
            lines.push_back(std::move(line));
    }
    if(input.bad())
        return absl::UnavailableError("manifest read failed");
    return lines;
}

absl::Status RepairTail(int fd)
{
    const auto size = ::lseek(fd, 0, SEEK_END);
    if(size < 0)
        return tier_detail::IoError("seek manifest");
    off_t end = size;
    char buffer[4096];
    while(end > 0)
    {
        const auto begin = std::max<off_t>(0, end - static_cast<off_t>(sizeof(buffer)));
        const auto n = ::pread(fd, buffer, static_cast<std::size_t>(end - begin), begin);
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
            return tier_detail::IoError("read manifest tail");
        for(auto i = n; i > 0; --i)
        {
            if(buffer[i - 1] == '\n')
            {
                const auto boundary = begin + i;
                if(boundary != size && ::ftruncate(fd, boundary) != 0)
                    return tier_detail::IoError("truncate manifest tail");
                return absl::OkStatus();
            }
        }
        end = begin;
    }
    if(size != 0 && ::ftruncate(fd, 0) != 0)
        return tier_detail::IoError("truncate manifest tail");
    return absl::OkStatus();
}
} // namespace

ManifestLog::ManifestLog(std::filesystem::path directory, std::string writer, int fd, PathStat path_stat)
    : directory_(std::move(directory))
    , writer_(std::move(writer))
    , fd_(fd)
    , path_stat_(path_stat ? std::move(path_stat)
                           : PathStat([](const std::filesystem::path& path, struct stat& info)
                                      { return ::stat(path.c_str(), &info); }))
{}

std::unique_ptr<ManifestLog> ManifestLog::OpenReadOnly(std::filesystem::path root, PathStat path_stat)
{
    return std::unique_ptr<ManifestLog>(new ManifestLog(root / "manifest", "", -1, std::move(path_stat)));
}

ManifestLog::~ManifestLog()
{
    if(fd_ >= 0)
        ::close(fd_);
}

absl::StatusOr<std::unique_ptr<ManifestLog>>
ManifestLog::Open(std::filesystem::path root, std::string writer, PathStat path_stat)
{
    if(!SafeWriter(writer))
        return absl::InvalidArgumentError("invalid manifest writer");
    std::error_code error;
    const auto directory = root / "manifest";
    std::filesystem::create_directories(directory, error);
    if(error)
        return absl::UnavailableError(error.message());
    const int fd = ::open((directory / (writer + ".log")).c_str(), O_CREAT | O_RDWR | O_APPEND | O_CLOEXEC, 0644);
    if(fd < 0)
        return tier_detail::IoError("open writer log");
    auto result = std::unique_ptr<ManifestLog>(new ManifestLog(directory, std::move(writer), fd, std::move(path_stat)));
    if(::flock(fd, LOCK_EX | LOCK_NB) != 0)
        return absl::UnavailableError("manifest writer already active");
    const auto status = RepairTail(fd);
    if(!status.ok())
        return status;
    const auto synced = tier_detail::SyncDirectory(root);
    if(!synced.ok())
        return synced;
    const auto synced_manifest = tier_detail::SyncDirectory(directory);
    if(!synced_manifest.ok())
        return synced_manifest;
    return result;
}

std::filesystem::path ManifestLog::logPath() const { return directory_ / (writer_ + ".log"); }
std::filesystem::path ManifestLog::snapshotPath() const { return directory_ / (writer_ + ".snap"); }

absl::Status ManifestLog::appendLine(std::string line)
{
    line += '\n';
    const auto repaired = RepairTail(fd_);
    if(!repaired.ok())
        return repaired;
    const auto status = tier_detail::WriteAll(fd_, line);
    if(!status.ok())
        return status;
    if(::fsync(fd_) != 0)
        return tier_detail::IoError("fsync manifest");
    return absl::OkStatus();
}

absl::Status ManifestLog::append(ManifestRecord record, std::optional<PhysicalBounds> bounds)
{
    std::lock_guard lock(mutex_);
    record.manifest_writer = writer_;
    try
    {
        const auto json = Encode(record, bounds);
        (void)Decode(json);
        (void)DecodeBounds(json, record.state);
        return appendLine(json.dump());
    }
    catch(const std::exception& error)
    {
        return absl::InvalidArgumentError(error.what());
    }
}

absl::Status ManifestLog::rememberWatermark(StoryId story, Hlc watermark)
{
    std::lock_guard lock(mutex_);
    return appendLine(Json{{"watermark", {watermark.physical_ns, watermark.logical}}, {"story", story}}.dump());
}

absl::Status ManifestLog::appendTombstone(StoryId story)
{
    std::lock_guard lock(mutex_);
    return appendLine(Json{{"tombstoned", true}, {"story", story}}.dump());
}

absl::Status ManifestLog::applyLine(const std::string& writer, const std::string& line, ManifestIndex& index) const
{
    try
    {
        const auto json = Json::parse(line);
        if(json.contains("watermark"))
        {
            Hlc value{json.at("watermark").at(0).get<int64_t>(), json.at("watermark").at(1).get<uint32_t>()};
            const auto story = json.at("story").get<StoryId>();
            auto [it, inserted] = index.watermarks.emplace(story, value);
            if(!inserted)
                it->second = std::max(it->second, value);
            return absl::OkStatus();
        }
        if(json.contains("tombstoned"))
        {
            const auto story = json.at("story").get<StoryId>();
            if(!story)
                return absl::UnavailableError("invalid tombstoned story");
            index.tombstoned.insert(story);
            return absl::OkStatus();
        }
        auto record = Decode(json);
        if(record.manifest_writer != writer)
            return absl::UnavailableError("foreign writer in manifest");
        const auto bounds = DecodeBounds(json, record.state);
        if(bounds)
            index.physical_bounds[record.file] = {*bounds,
                                                  record.story_id,
                                                  record.start,
                                                  record.end,
                                                  record.event_count};
        else
            index.physical_bounds.erase(record.file);
        index.by_story[record.story_id].push_back(index.records.size());
        if(!record.physical_policy)
            index.without_physical_policy.insert(record.story_id);
        index.records.push_back(std::move(record));
        return absl::OkStatus();
    }
    catch(const std::exception& exception)
    {
        return absl::UnavailableError(exception.what());
    }
}

namespace
{
// Bytes kept from just before a cursor. A log truncated and grown back past the cursor (same inode, size at least the
// cursor) no longer holds them there, so the reader starts over instead of reading from the middle of a line.
constexpr size_t kFingerprint = 4096;

absl::StatusOr<std::string> ReadRange(int fd, off_t begin, off_t end)
{
    std::string data(static_cast<size_t>(end - begin), '\0');
    size_t read = 0;
    while(read < data.size())
    {
        const auto n = ::pread(fd, data.data() + read, data.size() - read, begin + static_cast<off_t>(read));
        if(n < 0 && errno == EINTR)
            continue;
        if(n < 0)
            return tier_detail::IoError("read manifest");
        if(n == 0)
            return absl::UnavailableError("manifest shrank while it was read");
        read += static_cast<size_t>(n);
    }
    return data;
}

std::string Fingerprint(std::string previous, std::string_view appended)
{
    previous += appended;
    if(previous.size() > kFingerprint)
        previous.erase(0, previous.size() - kFingerprint);
    return previous;
}

// Opens before it decides anything: on NFS open revalidates the attribute cache (close-to-open) and fstat on the
// descriptor is then current, while a path stat can lag an append by acregmax. -1 with errno ENOENT when absent.
absl::StatusOr<int> OpenManifest(const std::filesystem::path& path, struct stat& info)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0)
    {
        if(errno == ENOENT)
            return -1;
        return tier_detail::IoError("open manifest " + path.string());
    }
    if(::fstat(fd, &info) != 0)
    {
        auto status = tier_detail::IoError("stat manifest");
        ::close(fd);
        return status;
    }
    return fd;
}

// Reads from `offset` to `size` and keeps only complete lines; returns the offset just past the last of them.
absl::StatusOr<std::pair<std::string, off_t>> ReadTail(int fd, off_t offset, off_t size)
{
    auto data = ReadRange(fd, offset, size);
    if(!data.ok())
        return data.status();
    const auto end = data->rfind('\n');
    data->resize(end == std::string::npos ? 0 : end + 1);
    const auto consumed = offset + static_cast<off_t>(data->size());
    return std::pair{*std::move(data), consumed};
}

template <class Fn>
absl::Status ForEachLine(const std::string& data, Fn&& fn)
{
    size_t begin = 0;
    while(begin < data.size())
    {
        const auto end = data.find('\n', begin);
        if(end > begin)
            if(auto status = fn(data.substr(begin, end - begin)); !status.ok())
                return status;
        begin = end + 1;
    }
    return absl::OkStatus();
}

absl::StatusOr<std::set<std::string>> Writers(const std::filesystem::path& directory)
{
    std::error_code error;
    std::set<std::string> writers;
    for(std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
        if(it->path().extension() == ".log" || it->path().extension() == ".snap")
            writers.insert(it->path().stem().string());
    if(error == std::errc::no_such_file_or_directory)
        return std::set<std::string>{};
    if(error)
        return absl::UnavailableError(error.message());
    return writers;
}
} // namespace

// Builds a whole index privately and installs it only when every file parsed, so a failed refresh leaves the
// previous index in place for concurrent readers.
absl::Status ManifestLog::rebuild() const
{
    auto writers = Writers(directory_);
    if(!writers.ok())
        return writers.status();
    ManifestIndex index;
    std::map<std::string, WriterCursors> cursors;
    for(const auto& writer: *writers)
    {
        std::set<std::string> seen;
        auto& writer_cursors = cursors[writer];
        for(const auto* extension: {".log", ".snap"})
        {
            struct stat info;
            auto opened = OpenManifest(directory_ / (writer + extension), info);
            if(!opened.ok())
                return opened.status();
            if(*opened < 0)
                continue;
            tier_detail::Fd fd(*opened);
            auto tail = ReadTail(fd.get(), 0, info.st_size);
            if(!tail.ok())
                return tail.status();
            auto& cursor = std::string(extension) == ".log" ? writer_cursors.log : writer_cursors.snapshot;
            cursor = {info.st_dev, info.st_ino, tail->second, true, Fingerprint({}, tail->first)};
            auto status = ForEachLine(tail->first,
                                      [&](std::string line)
                                      {
                                          if(!seen.insert(line).second)
                                              return absl::OkStatus();
                                          return applyLine(writer, line, index);
                                      });
            if(!status.ok())
                return status;
        }
    }
    index.generation = ++generations_;
    cache_ = std::move(index);
    cursors_ = std::move(cursors);
    return absl::OkStatus();
}

absl::Status ManifestLog::advance() const
{
    auto writers = Writers(directory_);
    if(!writers.ok())
        return writers.status();
    if(writers->size() != cursors_.size())
        return rebuild();
    for(const auto& writer: *writers)
        if(!cursors_.contains(writer))
            return rebuild();
    struct Pending
    {
        const std::string* writer;
        Cursor* cursor;
        Cursor next;
        std::string data;
    };
    std::vector<Pending> pending;
    for(auto& [writer, cursors]: cursors_)
    {
        for(const auto* extension: {".snap", ".log"})
        {
            auto& cursor = std::string(extension) == ".log" ? cursors.log : cursors.snapshot;
            struct stat info;
            auto opened = OpenManifest(directory_ / (writer + extension), info);
            if(!opened.ok())
                return opened.status();
            if(*opened < 0)
            {
                if(cursor.present)
                    return rebuild();
                continue;
            }
            tier_detail::Fd fd(*opened);
            if(!cursor.present || cursor.device != info.st_dev || cursor.inode != info.st_ino ||
               info.st_size < cursor.offset)
                return rebuild();
            if(std::string(extension) == ".snap" && info.st_size != cursor.offset)
                return rebuild();
            if(!cursor.fingerprint.empty())
            {
                auto prior = ReadRange(fd.get(),
                                       cursor.offset - static_cast<off_t>(cursor.fingerprint.size()),
                                       cursor.offset);
                if(!prior.ok())
                    return prior.status();
                if(*prior != cursor.fingerprint)
                    return rebuild();
            }
            if(info.st_size == cursor.offset)
                continue;
            auto tail = ReadTail(fd.get(), cursor.offset, info.st_size);
            if(!tail.ok())
                return tail.status();
            if(tail->first.empty())
                continue;
            // Every failure applyLine can report depends on the line alone, so a line that applies to a scratch index
            // applies to the cache too and the cache is never left half advanced.
            auto valid = ForEachLine(tail->first,
                                     [&](std::string line)
                                     {
                                         ManifestIndex scratch;
                                         return applyLine(writer, line, scratch);
                                     });
            if(!valid.ok())
                return valid;
            Cursor next = cursor;
            next.offset = tail->second;
            next.fingerprint = Fingerprint(cursor.fingerprint, tail->first);
            pending.push_back({&writer, &cursor, std::move(next), std::move(tail->first)});
        }
    }
    for(auto& update: pending)
    {
        auto status =
                ForEachLine(update.data, [&](std::string line) { return applyLine(*update.writer, line, cache_); });
        CHECK(status.ok()) << "manifest line failed after it validated: " << status;
        *update.cursor = std::move(update.next);
    }
    return absl::OkStatus();
}

absl::StatusOr<const ManifestIndex*> ManifestLog::sync() const
{
    std::lock_guard lock(mutex_);
    auto status = synced_ ? advance() : rebuild();
    if(!status.ok())
        return status;
    synced_ = true;
    return &cache_;
}

absl::StatusOr<ManifestIndex> ManifestLog::load() const
{
    auto index = sync();
    if(!index.ok())
        return index.status();
    std::lock_guard lock(mutex_);
    return **index;
}

absl::Status ManifestLog::compact()
{
    std::lock_guard lock(mutex_);
    std::vector<std::string> records;
    std::set<std::string> seen;
    for(const auto& path: {snapshotPath(), logPath()})
    {
        struct stat info;
        if(path_stat_(path, info) != 0)
        {
            if(errno == ENOENT)
                continue;
            return tier_detail::IoError("stat own manifest");
        }
        auto lines = ReadLines(path);
        if(!lines.ok())
            return lines.status();
        for(auto& line: *lines)
            if(seen.insert(line).second)
                records.push_back(std::move(line));
    }
    const auto temporary = snapshotPath().string() + ".tmp";
    tier_detail::Fd snapshot(::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644));
    if(snapshot.get() < 0)
        return tier_detail::IoError("open manifest snapshot");
    for(const auto& line: records)
    {
        const auto status = tier_detail::WriteAll(snapshot.get(), line + '\n');
        if(!status.ok())
            return status;
    }
    if(::fsync(snapshot.get()) != 0)
        return tier_detail::IoError("fsync manifest snapshot");
    if(::rename(temporary.c_str(), snapshotPath().c_str()) != 0)
        return tier_detail::IoError("rename manifest snapshot");
    const auto synced = tier_detail::SyncDirectory(directory_);
    if(!synced.ok())
        return synced;
    if(::ftruncate(fd_, 0) != 0 || ::fsync(fd_) != 0)
        return tier_detail::IoError("truncate compacted manifest");
    synced_ = false;
    return absl::OkStatus();
}
} // namespace chronolog
