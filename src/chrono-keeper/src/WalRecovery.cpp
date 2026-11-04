#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/uio.h>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <LogEventCodec.h>
#include <WalFormat.h>
#include <WalRecovery.h>

namespace chronolog
{

namespace
{

// Where a segment's records end, kept for the checks that need every segment
// read first: whether a bad frame or leftover bytes are a torn write depends
// on whether a newer segment holds data.
struct SegmentEnd
{
    uint64_t segment = 0;
    std::string name;
    uint64_t size = 0;
    uint64_t header_end = 0; // end of the H frame
    uint64_t data_end = 0;   // the zero length, the bad frame, or the end of the file
    bool bad_frame = false;
    std::string reason;                // the scan's reason for a bad frame
    std::optional<uint64_t> after_end; // first nonzero byte at or after data_end
    // End of the group the last whole G record starts; none in a segment
    // without one. after_group is the first nonzero byte at or after it, when
    // the data ends inside that group.
    std::optional<uint64_t> group_end;
    std::optional<uint64_t> after_group;

    bool dataAfterHeader() const { return data_end > header_end || after_end.has_value(); }
};

std::size_t firstNonZero(std::string_view bytes, std::size_t from)
{
    static char const zeros[4096] = {};
    for(std::size_t pos = from; pos < bytes.size(); pos += sizeof(zeros))
    {
        std::size_t const len = std::min(sizeof(zeros), bytes.size() - pos);
        if(std::memcmp(bytes.data() + pos, zeros, len) != 0)
        {
            std::size_t i = pos;
            while(bytes[i] == '\0') { ++i; }
            return i;
        }
    }
    return std::string_view::npos;
}

std::string segmentText(std::string const& name, std::string const& directory)
{
    return "WAL segment " + name + " in " + directory;
}

std::string offsetText(uint64_t offset) { return "at offset " + std::to_string(offset); }

std::string typeText(uint8_t type)
{
    char text[16];
    if(type >= 0x20 && type < 0x7F)
    {
        std::snprintf(text, sizeof(text), "0x%02x ('%c')", type, type);
    }
    else
    {
        std::snprintf(text, sizeof(text), "0x%02x", type);
    }
    return text;
}

RecoveredStory& storyOf(RecoveredState& state, StoryId story_id)
{
    auto [story, inserted] = state.stories.try_emplace(story_id);
    if(inserted)
    {
        story->second.wal_story_id = story_id;
    }
    return story->second;
}

// Replays one record after the H in log order. False, with error set, for a
// record this build cannot read.
bool applyRecord(wal::ScannedRecord const& record,
                 uint64_t segment,
                 std::string const& where,
                 RecoveredState& state,
                 std::string& error)
{
    switch(record.type)
    {
        case static_cast<uint8_t>(wal::RecordType::Names):
        {
            wal::NamesRecord names;
            if(wal::decodeNames(record.body, names) != CL_SUCCESS)
            {
                error = where + ": the N record " + offsetText(record.offset) + " does not decode";
                return false;
            }
            RecoveredStory& story = storyOf(state, names.story_id);
            if(story.has_names && (story.chronicle != names.chronicle || story.story != names.story))
            {
                LOG_WARNING("[WalRecovery] {}: story id {} is named {}/{} {}, after {}/{}; keeping the later names",
                            where,
                            names.story_id,
                            names.chronicle,
                            names.story,
                            offsetText(record.offset),
                            story.chronicle,
                            story.story);
            }
            story.has_names = true;
            story.chronicle = std::move(names.chronicle);
            story.story = std::move(names.story);
            return true;
        }
        case static_cast<uint8_t>(wal::RecordType::Event):
        {
            LogEvent event;
            // unknown extension tags are skipped: none is used yet
            CodecStatus status = decodeLogEvent(record.body.data() + 1, record.body.size() - 1, event);
            if(status == CodecStatus::UnsupportedVersion)
            {
                error = where + ": the E record " + offsetText(record.offset) + " has event codec version " +
                        std::to_string(static_cast<uint8_t>(record.body.size() > 1 ? record.body[1] : 0)) +
                        "; this build reads event codec version " + std::to_string(kLogEventCodecVersion);
                return false;
            }
            if(status != CodecStatus::Ok)
            {
                error = where + ": the E record " + offsetText(record.offset) + " does not decode (" +
                        (status == CodecStatus::Truncated ? "cut short" : "a length over the limit") + ")";
                return false;
            }
            EventSequence const seq{event.time(), event.getClientId(), event.index()};
            RecoveredStory& story = storyOf(state, event.storyId);
            // copies of one identity collapse; the first one is replayed
            story.events.emplace(seq, std::move(event));
            story.copies[seq].emplace_back(segment, wal::kFrameHeaderBytes + record.body.size());
            return true;
        }
        case static_cast<uint8_t>(wal::RecordType::Dropped):
        {
            wal::DroppedRecord dropped;
            if(wal::decodeDropped(record.body, dropped) != CL_SUCCESS)
            {
                error = where + ": the D record " + offsetText(record.offset) + " does not decode";
                return false;
            }
            // events after the D belong to a story re-created under the same id;
            // the names stay, as the committer does not write them again in
            // this segment
            auto story = state.stories.find(dropped.story_id);
            if(story != state.stories.end())
            {
                story->second.events.clear();
                story->second.copies.clear();
            }
            // so the WAL can keep the destroy if it deletes this segment
            // before an older one
            state.dropped_in[segment].push_back(dropped.story_id);
            return true;
        }
        case static_cast<uint8_t>(wal::RecordType::Group):
        {
            // only marks where a group starts; recoverWal uses the last one
            wal::GroupRecord group{};
            if(wal::decodeGroup(record.body, group) != CL_SUCCESS || group.bytes < wal::kGroupFrameBytes)
            {
                error = where + ": the G record " + offsetText(record.offset) + " does not decode";
                return false;
            }
            return true;
        }
        case static_cast<uint8_t>(wal::RecordType::Header):
            error = where + ": a second H record " + offsetText(record.offset);
            return false;
        default:
            error = where + ": record type " + typeText(record.type) + " " + offsetText(record.offset) +
                    ", which this build does not know; this build reads H, N, E, D and G records";
            return false;
    }
}

// Reads one segment: checks its H record, replays the records before the end
// of its data, and fills `end` for the checks made once every segment is read.
bool readSegment(WalFileSink& sink,
                 std::string const& directory,
                 uint64_t segment,
                 RecoveredState& state,
                 SegmentEnd& end,
                 uint64_t& created_ns,
                 std::string& error)
{
    std::string const name = wal::segmentFileName(segment);
    std::string const where = segmentText(name, directory);
    std::string bytes;
    if(int err = sink.readFile(name, bytes))
    {
        error = "cannot read " + where + ": " + std::strerror(err);
        return false;
    }
    // the whole file, the pre-zeroed space after its records included
    state.bytes_read += bytes.size();
    wal::ScanResult const scan = wal::scanSegment(bytes);

    if(scan.records.empty() || scan.records[0].type != static_cast<uint8_t>(wal::RecordType::Header))
    {
        std::string why;
        if(!scan.records.empty())
        {
            why = "its first record is of type " + typeText(scan.records[0].type);
        }
        else if(scan.end == wal::ScanEnd::BadFrame)
        {
            why = "its first frame is bad: " + scan.reason;
        }
        else
        {
            why = bytes.empty() ? "the file is empty" : "it holds only zeros";
        }
        error = where + " does not start with an H record (" + why + "); it is not a segment this build wrote";
        return false;
    }
    wal::HeaderRecord header{};
    if(wal::decodeHeader(scan.records[0].body, header) != CL_SUCCESS)
    {
        if(header.format_version != 0 && header.format_version != wal::kFormatVersion)
        {
            error = where + " has WAL format version " + std::to_string(header.format_version) +
                    "; this build reads WAL format version " + std::to_string(wal::kFormatVersion) +
                    ". Nothing was changed: start the keeper with a build that reads it";
        }
        else
        {
            error = where + " has an H record this build cannot read (wrong magic, or fields cut short)";
        }
        return false;
    }
    if(header.segment_number != segment)
    {
        error = where + " has an H record for segment " + std::to_string(header.segment_number);
        return false;
    }
    created_ns = header.created_ns;

    end.segment = segment;
    end.name = name;
    end.size = bytes.size();
    end.header_end = wal::kFrameHeaderBytes + scan.records[0].body.size();
    end.data_end = scan.end_offset;
    end.bad_frame = (scan.end == wal::ScanEnd::BadFrame);
    end.reason = scan.reason;
    std::size_t const nonzero = firstNonZero(bytes, scan.end_offset);
    if(nonzero != std::string_view::npos)
    {
        end.after_end = nonzero;
    }
    for(std::size_t i = scan.records.size(); i-- > 1;)
    {
        wal::GroupRecord group{};
        if(scan.records[i].type == static_cast<uint8_t>(wal::RecordType::Group) &&
           wal::decodeGroup(scan.records[i].body, group) == CL_SUCCESS)
        {
            end.group_end = scan.records[i].offset + group.bytes;
            if(end.data_end < *end.group_end && *end.group_end < bytes.size())
            {
                std::size_t const after_group = firstNonZero(bytes, *end.group_end);
                if(after_group != std::string_view::npos)
                {
                    end.after_group = after_group;
                }
            }
            break;
        }
    }

    for(std::size_t i = 1; i < scan.records.size(); ++i)
    {
        if(!applyRecord(scan.records[i], segment, where, state, error))
        {
            return false;
        }
    }
    return true;
}

// The name of the copy of a torn tail that starts at `from`
std::string tailCopyName(std::string const& segment_name, uint64_t from)
{
    return segment_name + ".tail-" + std::to_string(from);
}

// Before a torn tail is zeroed: copies its bytes, from the end of the data to
// the last nonzero byte, to tailCopyName, and counts the whole records that
// follow the bad frame when its length can be trusted (nullopt when it cannot:
// a zero length, a length past the segment, or one over kMaxBodyBytes). Those
// records are lost to recovery; in a crash they belong to the group that was
// never acknowledged, but if the drive damaged a synced group they may be
// acknowledged events, and the copy is how to get them back.
int saveTornTail(WalFileSink& sink,
                 std::string const& name,
                 uint64_t from,
                 std::string const& copy,
                 uint64_t& copied,
                 std::optional<std::size_t>& whole_after)
{
    std::string bytes;
    if(int err = sink.readFile(name, bytes))
    {
        return err;
    }
    std::size_t last = bytes.size();
    while(last > from && bytes[last - 1] == '\0') { --last; }
    copied = last > from ? last - from : 0;
    if(bytes.size() >= from + wal::kFrameHeaderBytes)
    {
        uint64_t length = 0;
        for(int i = 3; i >= 0; --i)
        {
            length = (length << 8) | static_cast<unsigned char>(bytes[from + static_cast<std::size_t>(i)]);
        }
        uint64_t const next = from + wal::kFrameHeaderBytes + length;
        if(length != 0 && length <= wal::kMaxBodyBytes && next <= bytes.size())
        {
            whole_after = wal::scanSegment(std::string_view(bytes).substr(next)).records.size();
        }
    }
    return copied == 0 ? 0 : writeWholeFile(sink, copy, std::string_view(bytes).substr(from, copied));
}

// Overwrites [from, size) of the segment with zeros and data-syncs it. The file
// keeps its size, so the directory needs no sync.
int zeroTail(WalFileSink& sink, std::string const& name, uint64_t from, uint64_t size)
{
    int fd = -1;
    if(int err = sink.openForWrite(name, fd))
    {
        return err;
    }
    int err = from < size ? writeZeros(sink, fd, from, size - from) : 0;
    if(err == 0)
    {
        err = sink.syncData(fd);
    }
    // after the data sync, a close error says nothing about the zeros
    sink.closeFile(fd);
    return err;
}

// Data-syncs a segment without changing it.
int syncSegment(WalFileSink& sink, std::string const& name)
{
    int fd = -1;
    if(int err = sink.openForWrite(name, fd))
    {
        return err;
    }
    int err = sink.syncData(fd);
    sink.closeFile(fd);
    return err;
}

} // namespace

int recoverWal(WalFileSink& sink, WalSegmentManager& manager, RecoveredState& out, std::string& error)
{
    std::string const& directory = manager.directoryName();
    if(int err = manager.open())
    {
        error = manager.error().empty() ? "cannot open WAL directory " + directory + ": " + std::strerror(err)
                                        : manager.error();
        return CL_ERR_UNKNOWN;
    }

    RecoveredState state;
    state.segments = manager.segments();
    std::vector<SegmentEnd> ends(state.segments.size());
    uint64_t oldest_created_ns = 0;
    for(std::size_t i = 0; i < state.segments.size(); ++i)
    {
        uint64_t created_ns = 0;
        if(!readSegment(sink, directory, state.segments[i], state, ends[i], created_ns, error))
        {
            return CL_ERR_UNKNOWN;
        }
        if(i == 0)
        {
            oldest_created_ns = created_ns;
        }
    }

    // Groups are written in log order and each is synced before the next, so
    // only the last one can be torn, and it lies in the newest segment holding
    // anything after its H. A spare the preparer made after it holds only its
    // H. Anywhere else, a bad frame or bytes after the end of the data are
    // damage to records that may have been acknowledged.
    std::optional<std::size_t> newest;
    for(std::size_t i = ends.size(); i-- > 0;)
    {
        if(ends[i].dataAfterHeader())
        {
            newest = i;
            break;
        }
    }
    for(std::size_t i = 0; newest && i < *newest; ++i)
    {
        SegmentEnd const& end = ends[i];
        std::string const where = segmentText(end.name, directory);
        if(end.bad_frame)
        {
            error = where + " has a bad record: " + end.reason +
                    ". It is not the newest segment with data, so this is not a torn write; move the segment aside "
                    "to start without it";
            return CL_ERR_UNKNOWN;
        }
        if(end.after_end)
        {
            error = where + " holds data " + offsetText(*end.after_end) + " after the end of its records " +
                    offsetText(end.data_end) +
                    ". It is not the newest segment with data, so this is not a torn write; move the segment aside "
                    "to start without it";
            return CL_ERR_UNKNOWN;
        }
    }

    // In the newest segment, the bad frame or the bytes after the data must lie
    // in the last group. Data after the end of the group they lie in means a
    // later group was written, which happens only after this one synced: its
    // records may have been acknowledged, so this is damage, not a torn write.
    // A segment without G records, or whose last G record is itself lost, keeps
    // the rule above.
    if(newest)
    {
        SegmentEnd const& end = ends[*newest];
        std::string const where = segmentText(end.name, directory);
        bool const torn_tail = end.bad_frame || end.after_end.has_value();
        if(torn_tail && end.group_end && end.data_end > *end.group_end)
        {
            error = where + " holds records between offsets " + std::to_string(*end.group_end) + " and " +
                    std::to_string(end.data_end) + ", after the end of its last group and with no G record, " +
                    "then " +
                    (end.bad_frame ? "a bad record: " + end.reason : std::string("data after a zero length")) +
                    ". This is not a torn write; move the segment aside to start without it";
            return CL_ERR_UNKNOWN;
        }
        if(torn_tail && end.after_group)
        {
            error = where + " has " +
                    (end.bad_frame ? "a bad record: " + end.reason
                                   : "a zero length " + offsetText(end.data_end) + " followed by data") +
                    ", inside the group that ends " + offsetText(*end.group_end) + ", and data " +
                    offsetText(*end.after_group) +
                    " after that group. A later group was written, so this one had been synced and its records may "
                    "have been acknowledged: this is not a torn write. Move the segment aside to start without it";
            return CL_ERR_UNKNOWN;
        }
    }

    // The keeper that wrote these segments may have died after writing its last
    // group and before that group's data sync returned. On the same boot the
    // group then reads back whole from the page cache, with good CRCs and a
    // clean tail, but is not on disk; yet the WAL treats every copy returned
    // here as durable and answers a retry of it CL_SUCCESS. So every segment
    // is synced before anything is returned. Only the newest segment with data
    // should hold such a group (each group is synced before the next is
    // written, and a rotation syncs first), but syncing a clean file costs one
    // flush and does not depend on that order. The torn segment, if any, is
    // synced with its zeros, after the others, so a failed sync refuses before
    // any file is changed.
    std::optional<std::size_t> torn;
    if(newest && ends[*newest].after_end)
    {
        torn = newest;
    }
    for(std::size_t i = 0; i < ends.size(); ++i)
    {
        if(torn && i == *torn)
        {
            continue;
        }
        if(int err = syncSegment(sink, ends[i].name))
        {
            error = "cannot sync " + segmentText(ends[i].name, directory) + ": " + std::strerror(err);
            return CL_ERR_UNKNOWN;
        }
    }

    if(torn)
    {
        // A torn group can leave a later page written after a missing one, so
        // everything from the end of the data goes, not just the bad frame.
        SegmentEnd& end = ends[*torn];
        std::string const where = segmentText(end.name, directory);
        std::string const why =
                end.bad_frame ? end.reason : "a zero length followed by data " + offsetText(*end.after_end);
        // a copy first, so records a damaged synced group still holds are not
        // gone with the zeros; a failed copy does not stop the start
        std::string const copy = tailCopyName(end.name, end.data_end);
        uint64_t copied = 0;
        std::optional<std::size_t> whole_after;
        int const copy_err = saveTornTail(sink, end.name, end.data_end, copy, copied, whole_after);
        if(copy_err != 0)
        {
            LOG_ERROR("[WalRecovery] Cannot save a copy of the torn tail of {} in {}/{}: {}. Zeroing it anyway",
                      where,
                      directory,
                      copy,
                      std::strerror(copy_err));
        }
        if(int err = zeroTail(sink, end.name, end.data_end, end.size))
        {
            error = "cannot zero the torn tail of " + where + " from " + offsetText(end.data_end) + ": " +
                    std::strerror(err);
            return CL_ERR_UNKNOWN;
        }
        std::string const kept = copy_err == 0 ? ", copied to " + directory + "/" + copy : "";
        if(whole_after.value_or(0) > 0)
        {
            LOG_ERROR("[WalRecovery] Zeroed the tail of {}: {} bytes from offset {} ({}), and with it {} whole "
                      "record(s) after the bad one{}. After a crash they belong to a write that was never "
                      "acknowledged; if the drive damaged a synced write instead, they may be acknowledged events",
                      where,
                      end.size - end.data_end,
                      end.data_end,
                      why,
                      *whole_after,
                      kept);
        }
        else
        {
            LOG_WARNING("[WalRecovery] Zeroed the torn tail of {}: {} bytes from offset {} ({}){}",
                        where,
                        end.size - end.data_end,
                        end.data_end,
                        why,
                        kept);
        }
        end.after_end.reset();
    }

    // A spare renamed into place just before the crash may lack its directory
    // sync, and the WAL is about to write into it.
    if(!ends.empty())
    {
        if(int err = sink.syncDirectory())
        {
            error = "cannot sync WAL directory " + directory + ": " + std::strerror(err);
            return CL_ERR_UNKNOWN;
        }
    }

    if(!ends.empty() && !ends.back().dataAfterHeader())
    {
        state.reusable_spare = ends.back().segment;
    }

    // Destroys a failed WAL could not log as D records: each drops the
    // story's events whose every copy lies in a segment up to the one it
    // names. An entry whose segments are all gone has nothing left to drop.
    std::vector<UnloggedDrop> drops;
    if(int err = readUnloggedDrops(sink, drops))
    {
        error = "cannot read " + directory + "/" + kUnloggedDropsFile + ": " + std::strerror(err);
        return CL_ERR_UNKNOWN;
    }
    if(!drops.empty())
    {
        uint64_t const oldest_segment = state.segments.empty() ? UINT64_MAX : state.segments.front();
        std::vector<UnloggedDrop> still_needed;
        for(UnloggedDrop const& drop: drops)
        {
            if(drop.last_segment >= oldest_segment)
            {
                still_needed.push_back(drop);
            }
            auto story = state.stories.find(drop.story_id);
            if(story == state.stories.end())
            {
                continue;
            }
            std::size_t dropped = 0;
            for(auto copies = story->second.copies.begin(); copies != story->second.copies.end();)
            {
                bool const before_the_drop = std::all_of(copies->second.begin(),
                                                         copies->second.end(),
                                                         [&drop](std::pair<uint64_t, uint64_t> const& copy)
                                                         { return copy.first <= drop.last_segment; });
                if(!before_the_drop)
                {
                    ++copies;
                    continue;
                }
                story->second.events.erase(copies->first);
                copies = story->second.copies.erase(copies);
                ++dropped;
            }
            LOG_WARNING("[WalRecovery] StoryId={} was destroyed after the WAL failed: dropped {} of its events from "
                        "segments up to {}",
                        drop.story_id,
                        dropped,
                        drop.last_segment);
        }
        if(still_needed.size() != drops.size())
        {
            if(int err = writeUnloggedDrops(sink, still_needed))
            {
                // the stale entries only drop events from segments that are gone
                LOG_WARNING("[WalRecovery] Could not prune {}/{}: {}",
                            directory,
                            kUnloggedDropsFile,
                            std::strerror(err));
            }
        }
    }

    for(auto story = state.stories.begin(); story != state.stories.end();)
    {
        story = story->second.events.empty() ? state.stories.erase(story) : std::next(story);
    }

    // from the oldest H, written when that segment was prepared as a spare,
    // which can be well before its first record
    if(!state.segments.empty())
    {
        uint64_t const now_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                              std::chrono::system_clock::now().time_since_epoch())
                                                              .count());
        state.oldest_segment_age_secs = now_ns > oldest_created_ns ? (now_ns - oldest_created_ns) / 1000000000ull : 0;
    }

    out = std::move(state);
    error.clear();
    return CL_SUCCESS;
}

} // namespace chronolog
