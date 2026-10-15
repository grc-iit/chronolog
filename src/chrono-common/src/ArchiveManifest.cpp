#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <json-c/json.h>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>

#include <ArchiveManifest.h>

namespace fs = std::filesystem;
namespace chl = chronolog;

namespace
{
constexpr char kLogSuffix[] = ".log";
// how much of a log's first line identifies it: all of it, up to a size
// no record reaches (a publish record names a file no other log has)
constexpr std::size_t kHeadBytes = 64 * 1024;

bool getString(json_object* object, char const* key, std::string& value, bool required)
{
    json_object* field = nullptr;
    if(!json_object_object_get_ex(object, key, &field) || field == nullptr)
    {
        return !required;
    }
    if(!json_object_is_type(field, json_type_string))
    {
        return false;
    }
    value.assign(json_object_get_string(field), json_object_get_string_len(field));
    return true;
}

// Numbers are read and written as int64, not uint64: the uint64 calls need
// json-c 0.14, and the packages promise 0.13. Times in ns fit until 2262.
bool getUint64(json_object* object, char const* key, uint64_t& value, bool required)
{
    json_object* field = nullptr;
    if(!json_object_object_get_ex(object, key, &field) || field == nullptr)
    {
        return !required;
    }
    if(!json_object_is_type(field, json_type_int))
    {
        return false;
    }
    int64_t const number = json_object_get_int64(field);
    if(number < 0)
    {
        return false;
    }
    value = static_cast<uint64_t>(number);
    return true;
}

json_object* newUint64(uint64_t value) { return json_object_new_int64(static_cast<int64_t>(value)); }

// Where the first JSON object of a line ends: the index just past its closing
// brace, or npos. json_tokener_get_parse_end would say the same, but needs
// json-c 0.15.
std::size_t endOfFirstObject(std::string const& line)
{
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for(std::size_t i = 0; i < line.size(); ++i)
    {
        char const c = line[i];
        if(in_string)
        {
            if(escaped)
            {
                escaped = false;
            }
            else if(c == '\\')
            {
                escaped = true;
            }
            else if(c == '"')
            {
                in_string = false;
            }
        }
        else if(c == '"')
        {
            in_string = true;
        }
        else if(c == '{')
        {
            ++depth;
        }
        else if(c == '}' && --depth == 0)
        {
            return i + 1;
        }
    }
    return std::string::npos;
}

bool getBool(json_object* object, char const* key, bool& value)
{
    json_object* field = nullptr;
    if(!json_object_object_get_ex(object, key, &field) || field == nullptr)
    {
        return true;
    }
    if(!json_object_is_type(field, json_type_boolean))
    {
        return false;
    }
    value = json_object_get_boolean(field);
    return true;
}

json_object* newString(std::string const& value)
{
    return json_object_new_string_len(value.data(), static_cast<int>(value.size()));
}

// write() all of buffer, retrying short writes and EINTR
bool writeAll(int fd, std::string const& buffer)
{
    std::size_t written = 0;
    while(written < buffer.size())
    {
        ssize_t const n = ::write(fd, buffer.data() + written, buffer.size() - written);
        if(n < 0)
        {
            if(errno == EINTR)
            {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

bool endsWithoutNewline(std::string const& path)
{
    int const fd = ::open(path.c_str(), O_RDONLY);
    if(fd < 0)
    {
        return false;
    }
    bool partial = false;
    struct stat st = {};
    if(::fstat(fd, &st) == 0 && st.st_size > 0)
    {
        char last = '\n';
        partial = ::pread(fd, &last, 1, st.st_size - 1) == 1 && last != '\n';
    }
    ::close(fd);
    return partial;
}
} // namespace

std::string chl::toManifestLine(ArchiveManifestRecord const& record)
{
    json_object* object = json_object_new_object();
    if(record.op == ArchiveManifestRecord::Op::OPEN)
    {
        json_object_object_add(object, "op", json_object_new_string("open"));
        json_object_object_add(object, "writer", newString(record.writer));
        json_object_object_add(object, "time", newUint64(record.start));
        std::string line =
                json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
        json_object_put(object);
        return line;
    }
    bool const publish = record.op == ArchiveManifestRecord::Op::PUBLISH;
    json_object_object_add(object, "op", json_object_new_string(publish ? "publish" : "delete"));
    json_object_object_add(object, "chronicle", newString(record.chronicle));
    if(publish)
    {
        json_object_object_add(object, "story", newString(record.story));
        json_object_object_add(object, "file", newString(record.file));
        json_object_object_add(object, "start", newUint64(record.start));
        json_object_object_add(object, "end", newUint64(record.end));
        json_object_object_add(object, "events", newUint64(record.events));
    }
    else if(record.whole_chronicle)
    {
        json_object_object_add(object, "whole_chronicle", json_object_new_boolean(1));
    }
    else
    {
        json_object_object_add(object, "story", newString(record.story));
    }
    if(!publish && record.incarnation_bound != UINT64_MAX)
    {
        json_object_object_add(object, "writer_start", newUint64(record.writer_start));
        json_object_object_add(object, "up_to_incarnation", newUint64(record.incarnation_bound));
    }
    // PLAIN keeps it on one line; json-c escapes a newline inside a name
    std::string line = json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
    json_object_put(object);
    return line;
}

bool chl::parseManifestLine(std::string const& line, ArchiveManifestRecord& record)
{
    if(line.empty())
    {
        return false;
    }
    json_tokener* tokener = json_tokener_new();
    json_object* object = json_tokener_parse_ex(tokener, line.c_str(), static_cast<int>(line.size()));
    // A cut-short line leaves the tokener waiting for more, not in success.
    // The tokener stops after the first object, so anything after it (two
    // records run together) makes the line unreadable too.
    bool const complete = object != nullptr && json_tokener_get_error(tokener) == json_tokener_success &&
                          json_object_is_type(object, json_type_object) && endOfFirstObject(line) == line.size();
    json_tokener_free(tokener);
    if(!complete)
    {
        json_object_put(object);
        return false;
    }

    ArchiveManifestRecord parsed;
    std::string op;
    bool ok = getString(object, "op", op, true);
    if(ok && op != "open")
    {
        ok = getString(object, "chronicle", parsed.chronicle, true);
    }
    if(ok && op == "publish")
    {
        parsed.op = ArchiveManifestRecord::Op::PUBLISH;
        ok = getString(object, "story", parsed.story, true) && getString(object, "file", parsed.file, true) &&
             getUint64(object, "start", parsed.start, true) && getUint64(object, "end", parsed.end, true) &&
             getUint64(object, "events", parsed.events, false) && !parsed.file.empty();
    }
    else if(ok && op == "open")
    {
        parsed.op = ArchiveManifestRecord::Op::OPEN;
        ok = getString(object, "writer", parsed.writer, true) && getUint64(object, "time", parsed.start, true);
    }
    else if(ok && op == "delete")
    {
        parsed.op = ArchiveManifestRecord::Op::DELETE;
        ok = getBool(object, "whole_chronicle", parsed.whole_chronicle) &&
             getString(object, "story", parsed.story, !parsed.whole_chronicle) &&
             getUint64(object, "writer_start", parsed.writer_start, false) &&
             getUint64(object, "up_to_incarnation", parsed.incarnation_bound, false);
    }
    else
    {
        ok = false;
    }
    json_object_put(object);
    if(ok)
    {
        record = std::move(parsed);
    }
    return ok;
}

std::vector<std::string> chl::listArchiveManifestLogs(std::string const& archive_root, bool* listing_failed)
{
    std::vector<std::string> logs;
    fs::path const manifest_dir = fs::path(archive_root) / kArchiveManifestDirName;
    std::error_code ec;
    // increment(ec), not a range-for: a read error part way through the
    // listing would otherwise throw
    for(fs::directory_iterator it(manifest_dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code type_ec;
        if(it->path().extension() == kLogSuffix && it->is_regular_file(type_ec))
        {
            logs.push_back(it->path().string());
        }
    }
    bool const failed = ec && ec != std::errc::no_such_file_or_directory;
    if(failed)
    {
        LOG_ERROR("[ArchiveManifest] Could not list {}: {}", manifest_dir.string(), ec.message());
    }
    if(listing_failed != nullptr)
    {
        *listing_failed = failed;
    }
    std::sort(logs.begin(), logs.end());
    return logs;
}

chl::ArchiveManifestWriter::ArchiveManifestWriter(std::string const& archive_root, std::string const& writer_id)
    : theWriterId(writer_id)
    , theArchiveRoot(archive_root)
    , theManifestDir((fs::path(archive_root) / kArchiveManifestDirName).string())
    , theLogPath((fs::path(archive_root) / kArchiveManifestDirName / (writer_id + kLogSuffix)).string())
{}

// Creates the manifest directory, but not the archive root: a missing root is a
// misconfiguration or an archive file system that is not mounted, and creating
// it would put the archive on the local disk under the mount point.
bool chl::ArchiveManifestWriter::createManifestDirectory()
{
    std::error_code ec;
    if(!fs::is_directory(theArchiveRoot, ec))
    {
        LOG_ERROR("[ArchiveManifest] The archive directory {} does not exist", theArchiveRoot);
        return false;
    }
    fs::create_directory(theManifestDir, ec);
    if(ec)
    {
        LOG_ERROR("[ArchiveManifest] Could not create {}: {}", theManifestDir, ec.message());
        return false;
    }
    return true;
}

std::string chl::ArchiveManifestWriter::headerLine() const
{
    ArchiveManifestRecord header;
    header.op = ArchiveManifestRecord::Op::OPEN;
    header.writer = theWriterId;
    header.start = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
    return toManifestLine(header) + "\n";
}

int chl::ArchiveManifestWriter::open()
{
    std::lock_guard<std::mutex> lock(theMutex);
    if(!createManifestDirectory())
    {
        return CL_ERR_UNKNOWN;
    }
    int const fd = ::open(theLogPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if(fd < 0)
    {
        LOG_ERROR("[ArchiveManifest] Could not open {}: {}", theLogPath, std::strerror(errno));
        return CL_ERR_UNKNOWN;
    }
    // a new log starts with its header
    struct stat st = {};
    bool ok = ::fstat(fd, &st) == 0 && (st.st_size != 0 || writeAll(fd, headerLine()));
    ok = (::close(fd) == 0) && ok;
    if(!ok)
    {
        LOG_ERROR("[ArchiveManifest] Could not start {}: {}", theLogPath, std::strerror(errno));
        return CL_ERR_UNKNOWN;
    }
    theLogNeedsNewline = endsWithoutNewline(theLogPath);
    return CL_SUCCESS;
}

int chl::ArchiveManifestWriter::append(ArchiveManifestRecord const& record)
{
    // numbers are stored as int64 (see getUint64): a larger one would come
    // back negative and every reader would skip the record (UINT64_MAX as the
    // incarnation bound is not stored at all)
    constexpr uint64_t kMaxStored = static_cast<uint64_t>(INT64_MAX);
    bool const bound_stored = record.incarnation_bound != UINT64_MAX;
    if(record.start > kMaxStored || record.end > kMaxStored || record.events > kMaxStored ||
       (bound_stored && (record.incarnation_bound > kMaxStored || record.writer_start > kMaxStored)))
    {
        LOG_ERROR("[ArchiveManifest] Not appending a record for {}: a time or count does not fit in int64",
                  record.file);
        return CL_ERR_INVALID_ARG;
    }
    std::string line = toManifestLine(record) + "\n";
    std::lock_guard<std::mutex> lock(theMutex);
    int fd = ::open(theLogPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if(fd < 0 && errno == ENOENT)
    {
        // the manifest directory was removed under the running grapher: make
        // it again rather than fail every window from now on
        LOG_WARNING("[ArchiveManifest] {} is missing; creating it again", theManifestDir);
        if(createManifestDirectory())
        {
            fd = ::open(theLogPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
        }
        else
        {
            errno = ENOENT;
        }
    }
    if(fd < 0)
    {
        LOG_ERROR("[ArchiveManifest] Could not open {} to append: {}", theLogPath, std::strerror(errno));
        return CL_ERR_UNKNOWN;
    }
    // a log created again (its directory was removed) starts with a header; a
    // log that may end in a cut-short line gets the record on a line of its own
    struct stat st = {};
    if(::fstat(fd, &st) == 0 && st.st_size == 0)
    {
        line.insert(0, headerLine());
    }
    else if(theLogNeedsNewline)
    {
        line.insert(line.begin(), '\n');
    }
    bool const written = writeAll(fd, line);
    int const write_errno = errno;
    // close is where NFS sends what it buffered, and where it reports a failure
    bool const closed = ::close(fd) == 0;
    if(!written || !closed)
    {
        // some of the line may be on disk
        theLogNeedsNewline = true;
        LOG_ERROR("[ArchiveManifest] Could not append to {}: {}",
                  theLogPath,
                  std::strerror(written ? errno : write_errno));
        return CL_ERR_UNKNOWN;
    }
    theLogNeedsNewline = false;
    return CL_SUCCESS;
}

chl::ArchiveManifestTail::ArchiveManifestTail(std::string const& log_path)
    : theLogPath(log_path)
{}

int chl::ArchiveManifestTail::readNew(std::vector<ArchiveManifestRecord>& records, std::size_t max_bytes)
{
    // Open first and take the size from the descriptor: an NFS client
    // revalidates a file's cached attributes on open, while a stat of the path
    // can be answered from a cache up to acregmax old.
    int const fd = ::open(theLogPath.c_str(), O_RDONLY);
    if(fd < 0)
    {
        if(errno == ENOENT)
        {
            return CL_SUCCESS; // not created yet
        }
        LOG_ERROR("[ArchiveManifest] Could not open {}: {}", theLogPath, std::strerror(errno));
        return CL_ERR_UNKNOWN;
    }
    struct stat st = {};
    if(::fstat(fd, &st) != 0)
    {
        LOG_ERROR("[ArchiveManifest] Could not stat {}: {}", theLogPath, std::strerror(errno));
        ::close(fd);
        return CL_ERR_UNKNOWN;
    }
    auto const size = static_cast<uint64_t>(st.st_size);
    auto const inode = static_cast<uint64_t>(st.st_ino);
    // A replacement can reuse both the inode and the consumed byte count.
    // Check its header before treating an unchanged size as nothing new.
    if(inode != theInode || size < theOffset || !startsAsBefore(fd))
    {
        // another file under this name: everything in it is new
        theOffset = 0;
        theInode = inode;
        theHead.clear();
    }
    if(size <= theOffset)
    {
        ::close(fd);
        return CL_SUCCESS;
    }

    // Up to max_bytes, and past it only as far as the end of a line that does
    // not fit: a reader catching up on a long log holds one piece at a time.
    uint64_t const available = size - theOffset;
    std::string buffer;
    std::size_t last_newline = std::string::npos;
    uint64_t want = std::min<uint64_t>(available, std::max<std::size_t>(max_bytes, 1));
    while(true)
    {
        std::size_t got = buffer.size();
        buffer.resize(want);
        while(got < buffer.size())
        {
            ssize_t const n =
                    ::pread(fd, buffer.data() + got, buffer.size() - got, static_cast<off_t>(theOffset + got));
            if(n < 0 && errno == EINTR)
            {
                continue;
            }
            if(n < 0)
            {
                // nothing is consumed: the next call reads from the same offset
                LOG_ERROR("[ArchiveManifest] Could not read {}: {}", theLogPath, std::strerror(errno));
                ::close(fd);
                return CL_ERR_UNKNOWN;
            }
            if(n == 0)
            {
                break;
            }
            got += static_cast<std::size_t>(n);
        }
        bool const at_end = got < buffer.size() || got >= available;
        buffer.resize(got);
        last_newline = buffer.rfind('\n');
        if(last_newline != std::string::npos || at_end)
        {
            break;
        }
        want = std::min<uint64_t>(available, 2 * static_cast<uint64_t>(got));
    }
    ::close(fd);

    // whole lines only: a record after the last newline is still being written
    if(last_newline == std::string::npos)
    {
        return CL_SUCCESS;
    }
    std::string line;
    for(std::size_t begin = 0; begin <= last_newline;)
    {
        std::size_t const end = buffer.find('\n', begin);
        line.assign(buffer, begin, end - begin);
        begin = end + 1;
        ArchiveManifestRecord record;
        if(parseManifestLine(line, record))
        {
            if(record.op != ArchiveManifestRecord::Op::OPEN) // only marks the log's start
            {
                records.push_back(std::move(record));
            }
        }
        else if(!line.empty())
        {
            LOG_WARNING("[ArchiveManifest] Skipping an unreadable line in {}", theLogPath);
        }
    }
    if(theOffset == 0)
    {
        // the first line identifies this log (see startsAsBefore)
        theHead = buffer.substr(0, std::min(buffer.find('\n') + 1, kHeadBytes));
    }
    theOffset += last_newline + 1;
    return CL_SUCCESS;
}

bool chl::ArchiveManifestTail::startsAsBefore(int fd) const
{
    if(theOffset == 0 || theHead.empty())
    {
        return true;
    }
    std::string head(theHead.size(), '\0');
    ssize_t const n = ::pread(fd, head.data(), head.size(), 0);
    return n == static_cast<ssize_t>(head.size()) && head == theHead;
}
