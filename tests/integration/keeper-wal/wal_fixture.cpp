// Builds the WAL directories keeper_wal_startup_test.sh starts a real keeper
// on, and counts the event records in a keeper's WAL directory for
// keeper_wal_crash_test.sh.
//
//   wal_fixture --dir <path> --chronicle <name> --story <name>
//               --mode older-bad|last-group
//   wal_fixture --dir <path> --count-events
//
// older-bad writes two segments. Segment 1 has a record whose CRC is wrong and
// an intact record after it. Segment 2 has a valid event, so segment 1 is not
// the newest segment with data. Recovery must refuse to start and leave both
// files unchanged.
//
// last-group writes one segment. Its first group holds one event. Its last
// group has a record whose CRC is wrong and an intact record after that.
// Recovery keeps the first event, copies the tail and does not return the
// intact record that followed the bad one.
//
// Events are stamped 180 seconds before the fixture runs, so a grapher whose
// acceptance window is shorter than that can archive the kept event at once.
// KEPT and DROPPED lines are "<time> <client id> <index>".
//
// --count-events reads every "<n>.wal" segment in the directory up to its end
// of data or its first bad frame, and prints EVENTS <E records> and
// DUPLICATES <identities found in more than one E record>. A segment's size
// does not change as records are written, so this is how the crash test sees
// whether a resent identity was written a second time.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <tuple>
#include <stdexcept>
#include <string>

#include <LogEventCodec.h>
#include <StoryIdOf.h>
#include <WalFormat.h>
#include <chronolog_types.h>

namespace chl = chronolog;
namespace wal = chronolog::wal;
namespace fs = std::filesystem;

namespace
{

constexpr uint64_t kSegmentBytes = 64u << 10;
constexpr chl::ClientId kClient = 7;
constexpr char kEndpoint[] = "ofi+sockets://127.0.0.1:6666";

std::string frameOf(chl::LogEvent const& event)
{
    wal::EventFrame const frame = wal::encodeEvent(event);
    std::string out(frame.frame_header.data(), frame.frame_header.size());
    out += frame.head;
    out.append(frame.record.data(), frame.record.size());
    out += frame.tail;
    return out;
}

std::string groupOf(std::string const& frames)
{
    return wal::encodeGroup(wal::GroupRecord{wal::kGroupFrameBytes + frames.size()}) + frames;
}

std::string headerOf(uint64_t segment)
{
    return wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, segment, 1, kEndpoint});
}

void writeSegment(fs::path const& dir, uint64_t segment, std::string bytes)
{
    if(bytes.size() < kSegmentBytes)
    {
        bytes.resize(kSegmentBytes, '\0');
    }
    fs::path const path = dir / wal::segmentFileName(segment);
    std::ofstream out(path, std::ios::binary);
    if(!out)
    {
        throw std::runtime_error("cannot create " + path.string());
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if(!out)
    {
        throw std::runtime_error("cannot write " + path.string());
    }
}

chl::LogEvent eventOf(chl::StoryId story, uint64_t time, uint32_t index, std::string const& record)
{
    return chl::LogEvent(story, time, kClient, index, record);
}

void usage(char const* name)
{
    std::cerr << "Usage: " << name << " --dir <path> --chronicle <name> --story <name> --mode older-bad|last-group"
              << std::endl
              << "       " << name << " --dir <path> --count-events" << std::endl;
}

int countEvents(fs::path const& dir)
{
    using Identity = std::tuple<chl::StoryId, uint64_t, chl::ClientId, uint32_t>;
    std::map<Identity, long> seen;
    long events = 0;
    for(auto const& entry: fs::directory_iterator(dir))
    {
        if(!entry.is_regular_file() || !wal::parseSegmentFileName(entry.path().filename().string()))
        {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        if(!in)
        {
            throw std::runtime_error("cannot read " + entry.path().string());
        }
        // kept in a variable: the scanned records point into it
        std::string const bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        wal::ScanResult const scan = wal::scanSegment(bytes);
        for(wal::ScannedRecord const& record: scan.records)
        {
            if(record.type != static_cast<uint8_t>(wal::RecordType::Event))
            {
                continue;
            }
            chl::LogEvent event;
            if(chl::decodeLogEvent(record.body.data() + 1, record.body.size() - 1, event) != chl::CodecStatus::Ok)
            {
                throw std::runtime_error("an E record in " + entry.path().string() + " does not decode");
            }
            ++events;
            ++seen[Identity{event.getStoryId(), event.time(), event.getClientId(), event.index()}];
        }
    }
    long duplicates = 0;
    for(auto const& [identity, count]: seen)
    {
        if(count > 1)
        {
            ++duplicates;
        }
    }
    std::cout << "EVENTS " << events << std::endl;
    std::cout << "DUPLICATES " << duplicates << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string dir;
    std::string chronicle;
    std::string story;
    std::string mode;
    bool count_events = false;
    for(int i = 1; i < argc; ++i)
    {
        std::string const arg = argv[i];
        auto need = [&]() -> std::string
        {
            if(i + 1 >= argc)
            {
                usage(argv[0]);
                std::exit(2);
            }
            return argv[++i];
        };
        if(arg == "--dir")
        {
            dir = need();
        }
        else if(arg == "--chronicle")
        {
            chronicle = need();
        }
        else if(arg == "--story")
        {
            story = need();
        }
        else if(arg == "--mode")
        {
            mode = need();
        }
        else if(arg == "--count-events")
        {
            count_events = true;
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }
    if(count_events)
    {
        if(dir.empty() || !chronicle.empty() || !story.empty() || !mode.empty())
        {
            usage(argv[0]);
            return 2;
        }
        try
        {
            return countEvents(dir);
        }
        catch(std::exception const& ex)
        {
            std::cerr << "[wal_fixture] " << ex.what() << std::endl;
            return 1;
        }
    }
    if(dir.empty() || chronicle.empty() || story.empty() || (mode != "older-bad" && mode != "last-group"))
    {
        usage(argv[0]);
        return 2;
    }

    try
    {
        fs::create_directories(dir);
        chl::StoryId const story_id = chl::storyIdOf(chronicle, story);
        uint64_t const base = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count() -
                              180ULL * 1000000000ULL;
        std::string const names = wal::encodeNames(wal::NamesRecord{story_id, chronicle, story});

        if(mode == "last-group")
        {
            chl::LogEvent const kept = eventOf(story_id, base, 1, "kept-event");
            chl::LogEvent const torn = eventOf(story_id, base + 1, 2, "torn-event");
            chl::LogEvent const dropped = eventOf(story_id, base + 2, 3, "dropped-event");
            std::string torn_frame = frameOf(torn);
            torn_frame[torn_frame.size() - 5] ^= 0x01;
            writeSegment(dir, 1, headerOf(1) + groupOf(names + frameOf(kept)) + groupOf(torn_frame + frameOf(dropped)));
            std::cout << "KEPT " << kept.time() << " " << kept.getClientId() << " " << kept.index() << std::endl;
            std::cout << "DROPPED " << dropped.time() << " " << dropped.getClientId() << " " << dropped.index()
                      << std::endl;
        }
        else
        {
            chl::LogEvent const kept = eventOf(story_id, base, 1, "older-kept");
            chl::LogEvent const torn = eventOf(story_id, base + 1, 2, "older-torn");
            chl::LogEvent const after = eventOf(story_id, base + 2, 3, "older-after");
            chl::LogEvent const newer = eventOf(story_id, base + 3, 4, "newer-segment");
            std::string torn_frame = frameOf(torn);
            torn_frame[torn_frame.size() - 5] ^= 0x01;
            writeSegment(dir, 1, headerOf(1) + groupOf(names + frameOf(kept) + torn_frame + frameOf(after)));
            writeSegment(dir, 2, headerOf(2) + groupOf(names + frameOf(newer)));
        }
        std::cout << "MODE " << mode << std::endl;
    }
    catch(std::exception const& ex)
    {
        std::cerr << "[wal_fixture] " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
