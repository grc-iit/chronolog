#include "tier/ManifestLog.h"
#include "tier/FileIO.h"
#include <absl/crc/crc32c.h>
#include <absl/log/check.h>
#include <absl/log/log.h>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
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

Json Encode(const ManifestRecord& record,
            std::optional<PhysicalBounds> bounds,
            std::optional<FileChecksum> checksum = std::nullopt,
            uint64_t seq = 0)
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
    if(checksum)
    {
        json["bytes"] = checksum->bytes;
        json["crc32c"] = checksum->crc32c;
    }
    if(seq)
        json["seq"] = seq;
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

std::optional<FileChecksum> DecodeChecksum(const Json& json)
{
    if(!json.contains("bytes") && !json.contains("crc32c"))
        return std::nullopt;
    const auto& bytes = json.at("bytes");
    const auto& crc = json.at("crc32c");
    if(!bytes.is_number_unsigned() || !crc.is_number_unsigned() ||
       crc.get<uint64_t>() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("invalid archive checksum");
    return FileChecksum{bytes.get<uint64_t>(), crc.get<uint32_t>()};
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


std::string Hex(std::string_view input)
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

std::optional<std::string> Unhex(std::string_view input)
{
    if(input.empty() || input.size() % 2 != 0)
        return std::nullopt;
    std::string output;
    for(size_t i = 0; i < input.size(); i += 2)
    {
        unsigned byte = 0;
        const auto parsed = std::from_chars(input.data() + i, input.data() + i + 2, byte, 16);
        if(parsed.ec != std::errc{} || parsed.ptr != input.data() + i + 2)
            return std::nullopt;
        output += static_cast<char>(byte);
    }
    return output;
}

template <typename T>
bool Number(std::string_view text, T& value)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

// A framed line carries the byte length and CRC32C of its body ahead of it. Over NFS a long line can be visible with
// its final page and newline present while an earlier page is still a hole; such a line, or any line holding a NUL,
// is not yet complete and is retried at the next poll rather than failing the refresh.
constexpr std::string_view kFrame = "{\"length\":";
enum class Framing
{
    Plain,
    Incomplete,
    Framed
};

std::string Frame(std::string_view key, const std::string& body)
{
    return std::string(kFrame) + std::to_string(body.size()) +
           ",\"crc\":" + std::to_string(static_cast<uint32_t>(absl::ComputeCrc32c(body))) + ",\"" + std::string(key) +
           "\":" + body + "}";
}

Framing Unframe(std::string_view line, std::string_view& key, std::string_view& body)
{
    if(line.find('\0') != std::string_view::npos)
        return Framing::Incomplete;
    if(!line.starts_with(kFrame))
        return Framing::Plain;
    auto rest = line.substr(kFrame.size());
    auto field = [&rest](char end, std::string_view& value)
    {
        const auto at = rest.find(end);
        if(at == std::string_view::npos)
            return false;
        value = rest.substr(0, at);
        rest.remove_prefix(at + 1);
        return true;
    };
    std::string_view length_text, crc_text;
    size_t length = 0;
    uint32_t crc = 0;
    if(!field(',', length_text) || !Number(length_text, length) || !rest.starts_with("\"crc\":"))
        return Framing::Incomplete;
    rest.remove_prefix(6);
    if(!field(',', crc_text) || !Number(crc_text, crc) || !rest.starts_with('"'))
        return Framing::Incomplete;
    rest.remove_prefix(1);
    if(!field('"', key) || !rest.starts_with(':') || !rest.ends_with('}'))
        return Framing::Incomplete;
    body = rest.substr(1, rest.size() - 2);
    if(body.size() != length || static_cast<uint32_t>(absl::ComputeCrc32c(body)) != crc)
        return Framing::Incomplete;
    return Framing::Framed;
}

bool IncompleteLine(std::string_view line)
{
    std::string_view key, body;
    return Unframe(line, key, body) == Framing::Incomplete;
}

// Bytes of `data` (complete lines) before the first line that is not yet complete.
size_t CompletePrefix(const std::string& data)
{
    size_t begin = 0;
    while(begin < data.size())
    {
        const auto end = data.find('\n', begin);
        if(IncompleteLine(std::string_view(data).substr(begin, end - begin)))
            return begin;
        begin = end + 1;
    }
    return data.size();
}

Json EncodeHlc(Hlc value) { return {value.physical_ns, value.logical}; }
Hlc DecodeHlc(const Json& json) { return {json.at(0).get<int64_t>(), json.at(1).get<uint32_t>()}; }

bool StoryFile(const std::string& file, StoryId story)
{
    const std::filesystem::path path(file);
    return !file.empty() && !path.is_absolute() && path.parent_path() == std::to_string(story) &&
           path.filename() != "." && path.filename() != ".." && path.filename().string().front() != '.';
}

CompactionSwitch DecodeSwitch(const std::string& writer, const Json& json)
{
    CompactionSwitch change;
    change.writer = json.at("writer").get<std::string>();
    change.op = json.at("op").get<std::string>();
    change.story_id = json.at("story").get<StoryId>();
    change.w_floor = DecodeHlc(json.at("w_floor"));
    const auto& output = json.at("output");
    change.output = Decode(output);
    change.bounds = DecodeBounds(output, change.output.state);
    change.checksum = DecodeChecksum(output);
    change.seq = output.value("seq", uint64_t{0});
    if(change.writer != writer || change.output.manifest_writer != writer || !change.story_id ||
       change.output.story_id != change.story_id || change.output.state != ManifestState::Published ||
       change.output.exempt || change.output.chunk_id != change.op || change.op.empty() || change.op.size() > 64)
        throw std::runtime_error("invalid compaction output identity");
    const auto name = ParseCompactionOutput(change.output.file);
    if(!name || name->writer != writer || name->op != change.op || name->start != change.output.start ||
       name->end != change.output.end || !StoryFile(change.output.file, change.story_id))
        throw std::runtime_error("invalid compaction output name");
    const auto& inputs = json.at("inputs");
    if(!inputs.is_array() || inputs.size() < 2 || inputs.size() > 65536)
        throw std::runtime_error("invalid compaction input list");
    std::set<std::string> files;
    uint64_t total = 0;
    for(const auto& entry: inputs)
    {
        ManifestRecord input;
        input.chunk_id = entry.at("chunk").get<std::string>();
        input.manifest_writer = writer;
        input.file = entry.at("file").get<std::string>();
        input.story_id = change.story_id;
        input.start = DecodeHlc(entry.at("start"));
        input.end = DecodeHlc(entry.at("end"));
        input.event_count = entry.at("count").get<uint64_t>();
        input.physical_policy = entry.at("physical_policy").get<bool>();
        if(input.chunk_id.empty() || input.start >= input.end || !input.event_count || input.event_count > 65536 ||
           !StoryFile(input.file, change.story_id) || ParseCompactionOutput(input.file) ||
           !files.insert(input.file).second || input.physical_policy != change.output.physical_policy ||
           (!change.inputs.empty() && change.inputs.back().end != input.start))
            throw std::runtime_error("invalid compaction input");
        total += input.event_count;
        change.inputs.push_back(std::move(input));
    }
    if(change.inputs.front().start != change.output.start || change.inputs.back().end != change.output.end ||
       total != change.output.event_count)
        throw std::runtime_error("compaction output does not cover its inputs exactly");
    return change;
}

// A second switch naming an input or an output already claimed by a different switch is a conflict.
absl::Status SwitchConflict(const ManifestIndex& index, const CompactionSwitch& change)
{
    if(const auto found = index.switches.find(change.output.file); found != index.switches.end())
        return found->second.op == change.op ? absl::AlreadyExistsError("repeated compaction switch")
                                             : absl::UnavailableError("compaction output named twice");
    if(index.superseded.contains(change.output.file))
        return absl::UnavailableError("compaction output superseded");
    for(const auto& input: change.inputs)
        if(index.superseded.contains(input.file) || index.switches.contains(input.file))
            return absl::UnavailableError("compaction input superseded twice");
    return absl::OkStatus();
}

void ApplySwitch(CompactionSwitch change, ManifestIndex& index)
{
    const auto story = change.story_id;
    if(change.checksum)
        index.checksums[change.output.file] = *change.checksum;
    for(const auto& input: change.inputs) index.superseded[input.file] = change.output.file;
    if(change.bounds)
        index.physical_bounds[change.output.file] = {*change.bounds,
                                                     story,
                                                     change.output.start,
                                                     change.output.end,
                                                     change.output.event_count};
    if(!change.output.physical_policy)
        index.without_physical_policy.insert(story);
    auto [floor, inserted] = index.watermarks.emplace(story, change.w_floor);
    if(!inserted)
        floor->second = std::max(floor->second, change.w_floor);
    index.by_story[story].push_back(index.records.size());
    index.records.push_back(change.output);
    index.record_sequences[change.output.file] = change.seq;
    auto& highest = index.writer_sequences[change.writer];
    highest = std::max(highest, change.seq);
    ++index.revisions[story];
    index.switches.emplace(change.output.file, std::move(change));
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
// A final framed line whose body does not match its length and checksum was never completely written: the writer
// drops it before appending, as RepairTail drops a line without its newline.
absl::Status RepairIncompleteTail(int fd)
{
    const auto size = ::lseek(fd, 0, SEEK_END);
    if(size < 0)
        return tier_detail::IoError("seek manifest");
    std::string data(static_cast<size_t>(size), '\0');
    for(size_t read = 0; read < data.size();)
    {
        const auto n = ::pread(fd, data.data() + read, data.size() - read, static_cast<off_t>(read));
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
            return tier_detail::IoError("read manifest tail");
        read += static_cast<size_t>(n);
    }
    const auto complete = CompletePrefix(data);
    if(complete == data.size())
        return absl::OkStatus();
    if(data.find('\n', complete) + 1 != data.size())
    {
        LOG(ERROR) << "manifest holds an incomplete line before complete ones at byte " << complete;
        return absl::OkStatus();
    }
    if(::ftruncate(fd, static_cast<off_t>(complete)) != 0)
        return tier_detail::IoError("truncate manifest tail");
    return absl::OkStatus();
}
} // namespace

std::string CompactionOutputName(const CompactionOutput& output, const std::string& extension)
{
    return "compact-" + Hex(output.writer) + "-" + output.op + "_" + std::to_string(output.start.physical_ns) + "_" +
           std::to_string(output.start.logical) + "_" + std::to_string(output.end.physical_ns) + "_" +
           std::to_string(output.end.logical) + extension;
}

std::optional<CompactionOutput> ParseCompactionOutput(const std::filesystem::path& relative)
{
    const auto extension = relative.extension();
    const auto stem = relative.stem().string();
    if((extension != ".h5" && extension != ".pb") || !stem.starts_with("compact-"))
        return std::nullopt;
    const std::string_view rest = std::string_view(stem).substr(8);
    const auto dash = rest.find('-');
    if(dash == std::string_view::npos)
        return std::nullopt;
    auto writer = Unhex(rest.substr(0, dash));
    std::vector<std::string_view> fields;
    for(auto tail = rest.substr(dash + 1);;)
    {
        const auto at = tail.find('_');
        fields.push_back(tail.substr(0, at));
        if(at == std::string_view::npos)
            break;
        tail.remove_prefix(at + 1);
    }
    CompactionOutput output;
    if(!writer || !SafeWriter(*writer) || fields.size() != 5 || fields[0].empty() || fields[0].size() > 64 ||
       !std::all_of(fields[0].begin(),
                    fields[0].end(),
                    [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }) ||
       !Number(fields[1], output.start.physical_ns) || !Number(fields[2], output.start.logical) ||
       !Number(fields[3], output.end.physical_ns) || !Number(fields[4], output.end.logical) ||
       output.start >= output.end)
        return std::nullopt;
    output.writer = *writer;
    output.op = fields[0];
    return output;
}

std::string CompactionTemporaryPrefix(const std::string& writer) { return ".compact-" + Hex(writer) + "."; }

std::optional<std::string> ArchiveFileWriter(const std::filesystem::path& file)
{
    if(auto output = ParseCompactionOutput(file))
        return output->writer;
    if(file.extension() != ".pb" && file.extension() != ".h5")
        return std::nullopt;
    std::vector<std::string> fields;
    std::istringstream stream(file.stem().string());
    std::string field;
    while(std::getline(stream, field, '_')) fields.push_back(field);
    if(fields.size() != 8 && fields.size() != 9)
        return std::nullopt;
    return Unhex(fields[6]);
}

namespace
{
MigrationLocation DecodeMigration(const std::string& writer, const Json& json)
{
    MigrationLocation m;
    m.writer = json.at("writer").get<std::string>();
    m.file = json.at("file").get<std::string>();
    m.story_id = json.at("story").get<StoryId>();
    m.tier = json.at("tier").get<std::string>();
    m.rank = json.at("rank").get<uint32_t>();
    m.tier_uuid = json.at("tier_uuid").get<std::string>();
    m.token = json.at("token").get<std::string>();
    const auto checksum = DecodeChecksum(json);
    if(m.writer != writer || ArchiveFileWriter(m.file) != std::optional<std::string>(writer) ||
       !StoryFile(m.file, m.story_id) || !m.rank || !SafeWriter(m.tier) || m.tier == "local" || m.tier_uuid.empty() ||
       m.token.empty() || !checksum)
        throw std::runtime_error("invalid or foreign migration");
    m.checksum = *checksum;
    return m;
}
absl::Status MigrationConflict(const ManifestIndex& index, const MigrationLocation& m)
{
    const auto it = index.migration_ranks.find({m.file, m.rank});
    if(it != index.migration_ranks.end() && (it->second.tier != m.tier || it->second.tier_uuid != m.tier_uuid))
        return absl::UnavailableError("conflicting migration tier at the same rank");
    return absl::OkStatus();
}
void ApplyMigration(ManifestIndex& index, const MigrationLocation& m)
{
    index.migration_ranks[{m.file, m.rank}] = m;
    auto it = index.locations.find(m.file);
    if(it == index.locations.end() || it->second.rank < m.rank)
        index.locations[m.file] = m;
}
} // namespace

absl::Status ManifestLog::appendMigration(const MigrationLocation& m)
{
    try
    {
        Json body{{"writer", writer_},
                  {"story", m.story_id},
                  {"file", m.file},
                  {"tier", m.tier},
                  {"rank", m.rank},
                  {"tier_uuid", m.tier_uuid},
                  {"bytes", m.checksum.bytes},
                  {"crc32c", m.checksum.crc32c},
                  {"token", m.token}};
        (void)DecodeMigration(writer_, body);
        return appendFramed("migrate_v1", body.dump());
    }
    catch(const std::exception& error)
    {
        return absl::InvalidArgumentError(error.what());
    }
}

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
    auto status = RepairTail(fd);
    if(status.ok())
        status = RepairIncompleteTail(fd);
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
    auto status = RepairTail(fd_);
    if(status.ok())
        status = tier_detail::WriteAll(fd_, line);
    if(status.ok() && sync(fd_) != 0)
        status = tier_detail::IoError("fsync manifest");
    if(!status.ok())
        failed_ = true;
    else if(sequence_primed_)
        durable_seq_ = next_seq_;
    return status;
}

absl::Status ManifestLog::primeSequence()
{
    if(sequence_primed_)
        return absl::OkStatus();
    auto status = synced_ ? advance() : rebuild();
    if(!status.ok())
        return status;
    synced_ = true;
    next_seq_ = 0;
    if(const auto own = cache_.writer_sequences.find(writer_); own != cache_.writer_sequences.end())
        next_seq_ = own->second;
    // A seq at or below the mark is never assigned again, even when the line that carried it was lost to a torn
    // tail: the mark may already cover it.
    if(const auto mark = ValidatedMark(directory_.parent_path(), writer_))
        next_seq_ = std::max(next_seq_, *mark);
    sequence_primed_ = true;
    return absl::OkStatus();
}

uint64_t ManifestLog::durableSequence() const
{
    std::lock_guard lock(mutex_);
    return durable_seq_;
}

std::optional<uint64_t> ManifestLog::ValidatedMark(const std::filesystem::path& root, const std::string& writer)
{
    std::ifstream file(root / "manifest" / (writer + ".validated"));
    if(!file)
        return std::nullopt;
    try
    {
        Json mark;
        file >> mark;
        if(mark.at("writer").get<std::string>() != writer || !mark.at("through").is_number_unsigned())
            return std::nullopt;
        return mark.at("through").get<uint64_t>();
    }
    catch(const std::exception&)
    {
        return std::nullopt;
    }
}

absl::Status ManifestLog::writeValidatedMark(uint64_t through)
{
    std::lock_guard lock(mutex_);
    if(failed_.load())
        return absl::UnavailableError("manifest log failed; the validated mark stays");
    const auto path = directory_ / (writer_ + ".validated");
    const auto temporary = path.string() + ".tmp";
    tier_detail::Fd mark(::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644));
    if(mark.get() < 0)
        return tier_detail::IoError("open validated mark");
    auto status = tier_detail::WriteAll(mark.get(), Json{{"writer", writer_}, {"through", through}}.dump() + '\n');
    if(!status.ok())
        return status;
    if(::fsync(mark.get()) != 0)
        return tier_detail::IoError("fsync validated mark");
    if(::rename(temporary.c_str(), path.c_str()) != 0)
        return tier_detail::IoError("rename validated mark");
    return tier_detail::SyncDirectory(directory_);
}

void ManifestLog::setSync(std::function<int(int)> sync)
{
    std::lock_guard lock(mutex_);
    sync_ = std::move(sync);
}

absl::Status ManifestLog::syncOwn()
{
    std::lock_guard lock(mutex_);
    if(sync(fd_) == 0)
    {
        // Every own line read at open, and every seq assigned since, is durable from here on.
        if(primeSequence().ok())
            durable_seq_ = next_seq_;
        return absl::OkStatus();
    }
    failed_ = true;
    return tier_detail::IoError("fsync manifest");
}

absl::Status ManifestLog::appendFramed(std::string_view key, const std::string& body)
{
    const auto line = Frame(key, body);
    std::string_view parsed_key, parsed_body;
    if(Unframe(line, parsed_key, parsed_body) != Framing::Framed)
        return absl::InternalError("compaction line does not frame");
    std::lock_guard lock(mutex_);
    return appendLine(line);
}

absl::Status ManifestLog::appendSwitch(const CompactionSwitch& change)
{
    try
    {
        Json inputs = Json::array();
        for(const auto& input: change.inputs)
            inputs.push_back({{"chunk", input.chunk_id},
                              {"file", input.file},
                              {"start", EncodeHlc(input.start)},
                              {"end", EncodeHlc(input.end)},
                              {"count", input.event_count},
                              {"physical_policy", input.physical_policy}});
        auto output = change.output;
        output.manifest_writer = writer_;
        std::lock_guard lock(mutex_);
        if(auto primed = primeSequence(); !primed.ok())
            return primed;
        // The seq is consumed even when the append fails: a line that may be visible never lends it to another.
        const auto seq = ++next_seq_;
        const Json body = {{"op", change.op},
                           {"writer", writer_},
                           {"story", change.story_id},
                           {"inputs", std::move(inputs)},
                           {"output", Encode(output, change.bounds, change.checksum, seq)},
                           {"w_floor", EncodeHlc(change.w_floor)}};
        (void)DecodeSwitch(writer_, body);
        const auto line = Frame("compact_v1", body.dump());
        std::string_view parsed_key, parsed_body;
        if(Unframe(line, parsed_key, parsed_body) != Framing::Framed)
            return absl::InternalError("compaction line does not frame");
        return appendLine(line);
    }
    catch(const std::exception& error)
    {
        return absl::InvalidArgumentError(error.what());
    }
}

absl::Status ManifestLog::appendRollback(StoryId story, const std::string& output)
{
    const auto name = ParseCompactionOutput(output);
    if(!name || name->writer != writer_ || !StoryFile(output, story))
        return absl::InvalidArgumentError("rollback names no compaction output of this writer");
    return appendFramed("compact_rollback_v1", Json{{"story", story}, {"output", output}}.dump());
}

absl::Status
ManifestLog::append(ManifestRecord record, std::optional<PhysicalBounds> bounds, std::optional<FileChecksum> checksum)
{
    std::lock_guard lock(mutex_);
    record.manifest_writer = writer_;
    try
    {
        uint64_t seq = 0;
        if(record.state == ManifestState::Published || record.state == ManifestState::Empty)
        {
            if(auto primed = primeSequence(); !primed.ok())
                return primed;
            seq = ++next_seq_;
        }
        const auto json = Encode(record, bounds, checksum, seq);
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
        std::string_view key, body;
        const auto framing = Unframe(line, key, body);
        if(framing == Framing::Incomplete)
            return absl::UnavailableError("incomplete manifest line");
        if(framing == Framing::Framed)
        {
            const auto json = Json::parse(body);
            if(key == "migrate_v1")
            {
                auto m = DecodeMigration(writer, json);
                auto status = MigrationConflict(index, m);
                if(!status.ok())
                    return status;
                ApplyMigration(index, m);
                return absl::OkStatus();
            }
            if(key == "compact_v1")
            {
                auto change = DecodeSwitch(writer, json);
                const auto conflict = SwitchConflict(index, change);
                if(absl::IsAlreadyExists(conflict))
                    return absl::OkStatus();
                if(!conflict.ok())
                    return conflict;
                ApplySwitch(std::move(change), index);
                return absl::OkStatus();
            }
            if(key == "compact_rollback_v1")
            {
                const auto story = json.at("story").get<StoryId>();
                const auto output = json.at("output").get<std::string>();
                const auto name = ParseCompactionOutput(output);
                if(!story || !name || name->writer != writer || !StoryFile(output, story))
                    return absl::UnavailableError("invalid compaction rollback");
                index.rolled_back.insert(output);
                ++index.revisions[story];
                return absl::OkStatus();
            }
            return absl::UnavailableError("unknown framed manifest line");
        }
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
        // Only a line that makes the file effective names its seq; a later Lost or Deleted line keeps it.
        if(record.state == ManifestState::Published || record.state == ManifestState::Empty)
        {
            const auto seq = json.value("seq", uint64_t{0});
            index.record_sequences[record.file] = seq;
            auto& highest = index.writer_sequences[writer];
            highest = std::max(highest, seq);
        }
        const auto bounds = DecodeBounds(json, record.state);
        if(const auto checksum = DecodeChecksum(json))
            index.checksums[record.file] = *checksum;
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
    if(const auto complete = CompletePrefix(*data); complete != data->size())
    {
        LOG_EVERY_N_SEC(WARNING, 60) << "manifest line at byte " << offset + static_cast<off_t>(complete)
                                     << " is not complete yet; it is retried at the next poll";
        data->resize(complete);
    }
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
    std::map<std::string, std::string> claimed;
    ManifestIndex migrations;
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
            // A switch also must not conflict with the cache or with another switch read in this same advance.
            auto valid = ForEachLine(tail->first,
                                     [&](std::string line)
                                     {
                                         ManifestIndex scratch;
                                         auto status = applyLine(writer, line, scratch);
                                         if(!status.ok())
                                             return status;
                                         for(const auto& [file_rank, m]: scratch.migration_ranks)
                                         {
                                             auto conflict = MigrationConflict(cache_, m);
                                             if(!conflict.ok())
                                                 return conflict;
                                             conflict = MigrationConflict(migrations, m);
                                             if(!conflict.ok())
                                                 return conflict;
                                             ApplyMigration(migrations, m);
                                         }
                                         for(const auto& [output, change]: scratch.switches)
                                         {
                                             const auto conflict = SwitchConflict(cache_, change);
                                             if(absl::IsAlreadyExists(conflict))
                                                 continue;
                                             if(!conflict.ok())
                                                 return conflict;
                                             for(const auto& input: change.inputs)
                                                 if(const auto [it, inserted] = claimed.emplace(input.file, change.op);
                                                    !inserted && it->second != change.op)
                                                     return absl::UnavailableError("compaction input superseded twice");
                                             if(const auto [it, inserted] = claimed.emplace(output, change.op);
                                                !inserted && it->second != change.op)
                                                 return absl::UnavailableError("compaction output named twice");
                                         }
                                         return absl::OkStatus();
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

std::optional<MigrationLocation> ManifestLog::location(const std::string& file) const
{
    std::lock_guard lock(mutex_);
    const auto found = cache_.locations.find(file);
    return found == cache_.locations.end() ? std::nullopt : std::optional(found->second);
}

std::optional<FileChecksum> ManifestLog::checksum(const std::string& file) const
{
    std::lock_guard lock(mutex_);
    const auto found = cache_.checksums.find(file);
    return found == cache_.checksums.end() ? std::nullopt : std::optional(found->second);
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
    if(::ftruncate(fd_, 0) != 0 || sync(fd_) != 0)
    {
        failed_ = true;
        return tier_detail::IoError("truncate compacted manifest");
    }
    synced_ = false;
    return absl::OkStatus();
}
} // namespace chronolog
