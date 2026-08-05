#include <cstdio>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <sstream>

#include <json-c/json.h>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>

#include <ArchiveManifest.h>

namespace fs = std::filesystem;
namespace chl = chronolog;

namespace
{

constexpr char LOG_FILE_NAME[] = "archive_manifest.log";
constexpr char SNAPSHOT_FILE_NAME[] = "archive_manifest.json";
constexpr char SNAPSHOT_TMP_SUFFIX[] = ".tmp";

char const* stateToString(chl::ManifestState state)
{
    switch(state)
    {
        case chl::ManifestState::PUBLISHED:
            return "published";
        case chl::ManifestState::EMPTY:
            return "empty";
        case chl::ManifestState::DELETED:
            return "deleted";
        case chl::ManifestState::FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

bool stateFromString(std::string const& text, chl::ManifestState& out)
{
    if(text == "published")
    {
        out = chl::ManifestState::PUBLISHED;
        return true;
    }
    if(text == "empty")
    {
        out = chl::ManifestState::EMPTY;
        return true;
    }
    if(text == "deleted")
    {
        out = chl::ManifestState::DELETED;
        return true;
    }
    if(text == "failed")
    {
        out = chl::ManifestState::FAILED;
        return true;
    }
    return false;
}

// json-c hands back a borrowed object; every getter below is guarded because a
// torn line can parse as an object while missing arbitrary fields.
bool getString(json_object* obj, char const* key, std::string& out, bool required)
{
    json_object* val = nullptr;
    if(!json_object_object_get_ex(obj, key, &val) || val == nullptr)
    {
        return !required;
    }
    if(!json_object_is_type(val, json_type_string))
    {
        return false;
    }
    out = json_object_get_string(val);
    return true;
}

bool getUint64(json_object* obj, char const* key, uint64_t& out, bool required)
{
    json_object* val = nullptr;
    if(!json_object_object_get_ex(obj, key, &val) || val == nullptr)
    {
        return !required;
    }
    if(!json_object_is_type(val, json_type_int))
    {
        return false;
    }
    out = static_cast<uint64_t>(json_object_get_int64(val));
    return true;
}

// StoryId is written as a decimal string (see toManifestLine). A JSON integer is
// still accepted so a hand-written manifest loads, and it is read through
// get_uint64 so a negative two's-complement alias maps back to the same id.
bool getStoryId(json_object* obj, char const* key, chl::StoryId& out)
{
    json_object* val = nullptr;
    if(!json_object_object_get_ex(obj, key, &val) || val == nullptr)
    {
        return false;
    }
    if(json_object_is_type(val, json_type_string))
    {
        try
        {
            out = std::stoull(json_object_get_string(val));
        }
        catch(std::exception const&)
        {
            return false;
        }
        return true;
    }
    if(json_object_is_type(val, json_type_int))
    {
        out = static_cast<chl::StoryId>(json_object_get_int64(val));
        return true;
    }
    return false;
}

bool getBool(json_object* obj, char const* key, bool& out, bool required)
{
    json_object* val = nullptr;
    if(!json_object_object_get_ex(obj, key, &val) || val == nullptr)
    {
        return !required;
    }
    if(!json_object_is_type(val, json_type_boolean))
    {
        return false;
    }
    out = json_object_get_boolean(val);
    return true;
}

} // namespace

std::string chronolog::toManifestLine(chronolog::ArchiveManifestRecord const& record)
{
    json_object* obj = json_object_new_object();
    json_object_object_add(obj, "v", json_object_new_int64(record.version));
    json_object_object_add(obj, "chronicle", json_object_new_string(record.chronicle.c_str()));
    json_object_object_add(obj, "story", json_object_new_string(record.story.c_str()));
    // As a decimal string, not a JSON integer: StoryIds are CityHash64 outputs
    // and routinely exceed INT64_MAX, and JSON has no unsigned 64-bit type, so an
    // integer encoding writes them as negative two's-complement aliases. That
    // round-trips through this code but makes the manifest unreadable to anything
    // else -- jq, ops scripts, a human reading a bug report.
    json_object_object_add(obj, "story_id", json_object_new_string(std::to_string(record.story_id).c_str()));
    json_object_object_add(obj, "file", json_object_new_string(record.file.c_str()));
    json_object_object_add(obj, "start", json_object_new_int64(static_cast<int64_t>(record.start)));
    json_object_object_add(obj, "end", json_object_new_int64(static_cast<int64_t>(record.end)));
    json_object_object_add(obj, "seq", json_object_new_int64(record.seq));
    json_object_object_add(obj, "events", json_object_new_int64(static_cast<int64_t>(record.events)));
    json_object_object_add(obj, "state", json_object_new_string(stateToString(record.state)));
    json_object_object_add(obj, "exempt", json_object_new_boolean(record.exempt));

    // JSON_C_TO_STRING_PLAIN keeps the object on a single line, which the
    // newline-delimited log requires.
    std::string line = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
    json_object_put(obj);
    return line;
}

bool chronolog::parseManifestLine(std::string const& line, chronolog::ArchiveManifestRecord& out)
{
    if(line.empty())
    {
        return false;
    }

    json_tokener* tokener = json_tokener_new();
    json_object* obj = json_tokener_parse_ex(tokener, line.c_str(), static_cast<int>(line.size()));
    bool const complete = (obj != nullptr) && (json_tokener_get_error(tokener) == json_tokener_success);
    json_tokener_free(tokener);

    // json_tokener_parse_ex returns a partial object for a truncated document;
    // the error check above is what distinguishes a torn line from a whole one.
    if(!complete)
    {
        if(obj != nullptr)
        {
            json_object_put(obj);
        }
        return false;
    }
    if(!json_object_is_type(obj, json_type_object))
    {
        json_object_put(obj);
        return false;
    }

    ArchiveManifestRecord parsed;
    uint64_t version = ArchiveManifestRecord::CURRENT_VERSION;
    uint64_t seq = 0;
    std::string state_text = "published";

    bool ok = getUint64(obj, "v", version, false);
    ok = ok && getString(obj, "chronicle", parsed.chronicle, true);
    ok = ok && getString(obj, "story", parsed.story, true);
    ok = ok && getStoryId(obj, "story_id", parsed.story_id);
    ok = ok && getString(obj, "file", parsed.file, false);
    ok = ok && getUint64(obj, "start", parsed.start, true);
    ok = ok && getUint64(obj, "end", parsed.end, true);
    ok = ok && getUint64(obj, "seq", seq, false);
    ok = ok && getUint64(obj, "events", parsed.events, false);
    ok = ok && getString(obj, "state", state_text, false);
    ok = ok && getBool(obj, "exempt", parsed.exempt, false);
    ok = ok && stateFromString(state_text, parsed.state);

    json_object_put(obj);
    if(!ok)
    {
        return false;
    }

    parsed.version = static_cast<uint32_t>(version);
    parsed.seq = static_cast<uint32_t>(seq);
    out = parsed;
    return true;
}

std::vector<chronolog::ArchiveManifestRecord> chronolog::ArchiveManifest::records() const
{
    std::lock_guard<std::mutex> lock(theMutex);
    return theRecords;
}

chronolog::ArchiveManifest::ArchiveManifest(std::string const& archive_root,
                                            bool fsync_each_append,
                                            std::string const& writer_id)
    : theFsyncEachAppend(fsync_each_append)
    , theArchiveRoot(archive_root)
    , theWriterId(writer_id)
{
    if(writer_id.empty())
    {
        // No writer id: the unsuffixed pair. That is what a single-writer archive
        // and every pre-split archive already use, and what a reader compacting
        // nothing would fall back to. load() merges across writers either way.
        theLogPath = (fs::path(archive_root) / LOG_FILE_NAME).string();
        theSnapshotPath = (fs::path(archive_root) / SNAPSHOT_FILE_NAME).string();
    }
    else
    {
        theLogPath = (fs::path(archive_root) / ("archive_manifest." + writer_id + ".log")).string();
        theSnapshotPath = (fs::path(archive_root) / ("archive_manifest." + writer_id + ".json")).string();
    }
}

// Matches this writer's own pair, every other writer's, and the unsuffixed pair an
// archive written before the per-writer split still carries.
std::vector<std::string> chronolog::ArchiveManifest::discoverLogs(std::string const& archive_root)
{
    std::vector<std::string> logs;
    std::error_code ec;
    fs::directory_iterator dir(archive_root, ec);
    if(ec)
    {
        return logs;
    }
    for(auto const& entry: dir)
    {
        if(!entry.is_regular_file(ec))
        {
            continue;
        }
        std::string const name = entry.path().filename().string();
        if(name.rfind("archive_manifest", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0)
        {
            logs.push_back(entry.path().string());
        }
    }
    // Deterministic order so a merged read is reproducible.
    std::sort(logs.begin(), logs.end());
    return logs;
}

int chronolog::ArchiveManifest::append(chronolog::ArchiveManifestRecord const& record)
{
    std::lock_guard<std::mutex> lock(theMutex);
    std::ofstream log(theLogPath, std::ios::app);
    if(!log)
    {
        LOG_ERROR("[ArchiveManifest] Failed to open {} for append", theLogPath);
        return chronolog::CL_ERR_UNKNOWN;
    }
    log << toManifestLine(record) << "\n";
    log.flush();
    if(!log)
    {
        LOG_ERROR("[ArchiveManifest] Failed to append a record to {}", theLogPath);
        return chronolog::CL_ERR_UNKNOWN;
    }

    if(theFsyncEachAppend)
    {
        // ofstream::flush only pushes to the OS; the record is not on the platter
        // until the descriptor is synced.
        int const fd = ::open(theLogPath.c_str(), O_WRONLY | O_APPEND);
        if(fd >= 0)
        {
            ::fsync(fd);
            ::close(fd);
        }
        else
        {
            LOG_WARNING("[ArchiveManifest] manifest_fsync is on but {} could not be opened to sync", theLogPath);
        }
    }

    theRecords.push_back(record);
    theOwnRecords.push_back(record);
    return chronolog::CL_SUCCESS;
}

int chronolog::ArchiveManifest::load()
{
    std::lock_guard<std::mutex> lock(theMutex);
    theRecords.clear();
    theOwnRecords.clear();

    // Every writer's pair, snapshot before its log: a writer's log holds exactly
    // what it appended after its own last compaction. Readers merge across writers;
    // duplicate records between sources are harmless because deriveWatermarks
    // collapses identical windows and the Player's index dedups by file name.
    std::vector<std::string> sources;
    for(std::string const& log: discoverLogs(theArchiveRoot))
    {
        // <name>.log -> <name>.json
        sources.push_back(log.substr(0, log.size() - 4) + ".json");
        sources.push_back(log);
    }
    // Our own snapshot counts even if our log has been removed.
    if(std::find(sources.begin(), sources.end(), theSnapshotPath) == sources.end())
    {
        sources.push_back(theSnapshotPath);
    }

    std::size_t discarded = 0;
    for(std::string const& path: sources)
    {
        bool const own = (path == theLogPath || path == theSnapshotPath);
        std::ifstream input(path);
        if(!input)
        {
            continue; // absent is normal: no snapshot yet, or nothing appended
        }
        std::string line;
        while(std::getline(input, line))
        {
            if(line.empty())
            {
                continue;
            }
            ArchiveManifestRecord record;
            if(parseManifestLine(line, record))
            {
                theRecords.push_back(record);
                if(own)
                {
                    theOwnRecords.push_back(record);
                }
            }
            else
            {
                discarded++;
            }
        }
    }

    if(discarded > 0)
    {
        // Expected after a crash mid-append. Recovering fewer records only makes
        // W under-report, so this is a warning rather than a failure.
        LOG_WARNING("[ArchiveManifest] Discarded {} unparsable line(s) while loading {}; watermarks will be "
                    "restored from the {} record(s) that were complete",
                    discarded,
                    theArchiveRoot,
                    theRecords.size());
    }
    LOG_INFO("[ArchiveManifest] Loaded {} record(s) from {} manifest source(s) in {}",
             theRecords.size(),
             sources.size(),
             theArchiveRoot);
    return chronolog::CL_SUCCESS;
}

int chronolog::ArchiveManifest::snapshot()
{
    std::lock_guard<std::mutex> lock(theMutex);
    std::string const tmp_path = theSnapshotPath + SNAPSHOT_TMP_SUFFIX;
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if(!out)
        {
            LOG_ERROR("[ArchiveManifest] Failed to open {} for the snapshot", tmp_path);
            return chronolog::CL_ERR_UNKNOWN;
        }
        // theOwnRecords, not theRecords: after a merged load theRecords also holds
        // other writers' entries, and writing those into our snapshot while their
        // logs still carry them would duplicate a record on every compaction.
        for(ArchiveManifestRecord const& record: theOwnRecords) { out << toManifestLine(record) << "\n"; }
        out.flush();
        if(!out)
        {
            LOG_ERROR("[ArchiveManifest] Failed to write the snapshot {}", tmp_path);
            return chronolog::CL_ERR_UNKNOWN;
        }
    }

    // Atomic swap: after this rename a reader sees either the whole old snapshot
    // or the whole new one, never a partial file.
    std::error_code ec;
    fs::rename(tmp_path, theSnapshotPath, ec);
    if(ec)
    {
        LOG_ERROR("[ArchiveManifest] Failed to rename {} over {}: {}", tmp_path, theSnapshotPath, ec.message());
        fs::remove(tmp_path, ec);
        return chronolog::CL_ERR_UNKNOWN;
    }

    // Truncate only after the snapshot is in place: a crash before this point
    // leaves the log still holding everything the snapshot now covers, and the
    // loader tolerates the duplication because both describe the same windows.
    std::ofstream truncate_log(theLogPath, std::ios::trunc);
    if(!truncate_log)
    {
        LOG_WARNING("[ArchiveManifest] Snapshot written but the log {} could not be truncated", theLogPath);
    }

    LOG_INFO("[ArchiveManifest] Compacted {} record(s) into {}", theOwnRecords.size(), theSnapshotPath);
    return chronolog::CL_SUCCESS;
}

std::map<chronolog::StoryId, std::map<uint64_t, uint64_t>> chronolog::ArchiveManifest::persistedIntervals() const
{
    std::lock_guard<std::mutex> lock(theMutex);

    // The log is append-only, so a deletion arrives as a later record naming the
    // same file rather than by removing the earlier one. Collect those names
    // first: skipping only the DELETED record would leave the original
    // publication still counted, claiming a file that is gone from disk.
    std::set<std::string> deleted_files;
    for(ArchiveManifestRecord const& record: theRecords)
    {
        if(record.state == ManifestState::DELETED && !record.file.empty())
        {
            deleted_files.insert(record.file);
        }
    }

    // A std::map per story both sorts the windows and collapses repeats of the
    // same one (a re-send publishes the same interval again), keeping the widest
    // end.
    std::map<StoryId, std::map<uint64_t, uint64_t>> intervals;
    for(ArchiveManifestRecord const& record: theRecords)
    {
        if(record.exempt || record.end <= record.start)
        {
            continue;
        }
        if(record.state != ManifestState::PUBLISHED && record.state != ManifestState::EMPTY)
        {
            continue;
        }
        if(!record.file.empty() && deleted_files.count(record.file) > 0)
        {
            continue;
        }
        auto& story_intervals = intervals[record.story_id];
        auto emplaced = story_intervals.emplace(record.start, record.end);
        if(!emplaced.second && record.end > emplaced.first->second)
        {
            emplaced.first->second = record.end;
        }
    }
    return intervals;
}

std::map<chronolog::StoryId, uint64_t> chronolog::ArchiveManifest::deriveWatermarks() const
{
    // Same prefix rule the grapher's registry applies at runtime: anchor at the
    // story's earliest window and absorb while the next one starts at or below the
    // current W. One forward pass suffices because the intervals are sorted.
    std::map<StoryId, uint64_t> watermarks;
    for(auto const& story_entry: persistedIntervals())
    {
        auto const& story_intervals = story_entry.second;
        if(story_intervals.empty())
        {
            continue;
        }
        uint64_t w = story_intervals.begin()->first; // anchor
        for(auto const& interval: story_intervals)
        {
            if(interval.first > w)
            {
                break; // a gap: everything after it is parked, exactly as at runtime
            }
            if(interval.second > w)
            {
                w = interval.second;
            }
        }
        watermarks[story_entry.first] = w;
    }
    return watermarks;
}
