#include "chrono-grapher/tier/ManifestLog.h"
#include "chrono-grapher/tier/FileIO.h"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/file.h>

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

Json Encode(const ManifestRecord& record)
{
    return {{"chunk", record.chunk_id},
            {"writer", record.manifest_writer},
            {"file", record.file},
            {"story", record.story_id},
            {"start", {record.start.physical_ns, record.start.logical}},
            {"end", {record.end.physical_ns, record.end.logical}},
            {"count", record.event_count},
            {"state", static_cast<int>(record.state)},
            {"exempt", record.exempt}};
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

ManifestLog::ManifestLog(std::filesystem::path directory, std::string writer, int fd)
    : directory_(std::move(directory))
    , writer_(std::move(writer))
    , fd_(fd)
{}

ManifestLog::~ManifestLog()
{
    if(fd_ >= 0)
        ::close(fd_);
}

absl::StatusOr<std::unique_ptr<ManifestLog>> ManifestLog::Open(std::filesystem::path root, std::string writer)
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
    auto result = std::unique_ptr<ManifestLog>(new ManifestLog(directory, std::move(writer), fd));
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

absl::Status ManifestLog::append(ManifestRecord record)
{
    std::lock_guard lock(mutex_);
    record.manifest_writer = writer_;
    try
    {
        const auto json = Encode(record);
        (void)Decode(json);
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

absl::StatusOr<ManifestIndex> ManifestLog::load() const
{
    std::lock_guard lock(mutex_);
    ManifestIndex index;
    std::error_code error;
    std::set<std::string> writers;
    for(std::filesystem::directory_iterator it(directory_, error), end; !error && it != end; it.increment(error))
    {
        if(it->path().extension() == ".log" || it->path().extension() == ".snap")
            writers.insert(it->path().stem().string());
    }
    if(error)
        return absl::UnavailableError(error.message());
    try
    {
        for(const auto& writer: writers)
        {
            std::set<std::string> seen;
            for(const auto* extension: {".log", ".snap"})
            {
                const auto path = directory_ / (writer + extension);
                // open, rather than cached stat, discovers NFS changes.
                auto lines = ReadLines(path);
                if(!lines.ok())
                {
                    if(absl::IsNotFound(lines.status()))
                        continue;
                    return lines.status();
                }
                for(const auto& line: *lines)
                {
                    if(!seen.insert(line).second)
                        continue;
                    const auto json = Json::parse(line);
                    if(json.contains("watermark"))
                    {
                        Hlc value{json.at("watermark").at(0).get<int64_t>(),
                                  json.at("watermark").at(1).get<uint32_t>()};
                        const auto story = json.at("story").get<StoryId>();
                        auto [it, inserted] = index.watermarks.emplace(story, value);
                        if(!inserted)
                            it->second = std::max(it->second, value);
                    }
                    else
                    {
                        auto record = Decode(json);
                        if(record.manifest_writer != writer)
                            return absl::UnavailableError("foreign writer in manifest");
                        index.records.push_back(std::move(record));
                    }
                }
            }
        }
    }
    catch(const std::exception& exception)
    {
        return absl::UnavailableError(exception.what());
    }
    return index;
}

absl::Status ManifestLog::compact()
{
    std::lock_guard lock(mutex_);
    std::vector<std::string> records;
    std::set<std::string> seen;
    for(const auto& path: {snapshotPath(), logPath()})
    {
        if(::access(path.c_str(), F_OK) != 0)
        {
            if(errno == ENOENT)
                continue;
            return tier_detail::IoError("access own manifest");
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
    return absl::OkStatus();
}
} // namespace chronolog
