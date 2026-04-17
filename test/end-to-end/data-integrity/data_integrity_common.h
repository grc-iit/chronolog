// Shared helpers for end-to-end data-integrity tests.
//
// Three lenses validate three properties (StoryID consistency, event count,
// event order) against the same reference input that was injected by the
// fixture:
//   - CSV    – ChronoGrapher's per-chunk *.csv files in CSV_OUTPUT_DIR.
//   - HDF5   – ChronoStore's {chronicle}/{story}.h5 archives.
//   - Replay – events returned by chrono-client-admin -i then '-r ...'.
//
// Helpers below abstract the three lenses behind a common iterator-style
// "events" vector so each test binary can stay short and focused on its
// property check.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <getopt.h>
#include <unistd.h>

#include <StoryReader.h>
#include <StoryChunk.h>
#include <chrono_monitor.h>

#include "city.h"

namespace data_integrity
{

enum class Lens
{
    CSV,
    HDF5,
    REPLAY
};

inline const char* lens_name(Lens l)
{
    switch(l)
    {
        case Lens::CSV:
            return "CSV";
        case Lens::HDF5:
            return "HDF5";
        case Lens::REPLAY:
            return "Replay";
    }
    return "?";
}

struct Event
{
    uint64_t storyId = 0;
    uint64_t eventTime = 0;
    uint64_t clientId = 0;
    uint32_t eventIndex = 0;
    std::string logRecord;

    bool operator<(Event const& other) const
    {
        if(eventTime != other.eventTime)
            return eventTime < other.eventTime;
        if(clientId != other.clientId)
            return clientId < other.clientId;
        return eventIndex < other.eventIndex;
    }
};

struct Args
{
    Lens lens = Lens::CSV;
    std::string csvDir;
    std::string hdf5Dir;
    std::string referenceFile;
    std::string chronicle = "chronicle_0_0";
    std::string story = "story_0_0";
    std::string adminBin;
    std::string clientConf;
};

inline void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0
              << " --lens csv|hdf5|replay --reference <file>"
                 " [--csv-dir <dir>] [--hdf5-dir <dir>]"
                 " [--chronicle <name>] [--story <name>]"
                 " [--admin-bin <path>] [--client-conf <path>]\n";
}

inline bool parse_args(int argc, char** argv, Args& out)
{
    static option longopts[] = {{"lens", required_argument, nullptr, 'L'},
                                {"csv-dir", required_argument, nullptr, 'C'},
                                {"hdf5-dir", required_argument, nullptr, 'H'},
                                {"reference", required_argument, nullptr, 'R'},
                                {"chronicle", required_argument, nullptr, 'c'},
                                {"story", required_argument, nullptr, 's'},
                                {"admin-bin", required_argument, nullptr, 'A'},
                                {"client-conf", required_argument, nullptr, 'K'},
                                {"help", no_argument, nullptr, 'h'},
                                {nullptr, 0, nullptr, 0}};
    int opt;
    while((opt = getopt_long(argc, argv, "L:C:H:R:c:s:A:K:h", longopts, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'L':
            {
                std::string v = optarg;
                if(v == "csv")
                    out.lens = Lens::CSV;
                else if(v == "hdf5")
                    out.lens = Lens::HDF5;
                else if(v == "replay")
                    out.lens = Lens::REPLAY;
                else
                {
                    std::cerr << "Unknown lens: " << v << "\n";
                    return false;
                }
                break;
            }
            case 'C':
                out.csvDir = optarg;
                break;
            case 'H':
                out.hdf5Dir = optarg;
                break;
            case 'R':
                out.referenceFile = optarg;
                break;
            case 'c':
                out.chronicle = optarg;
                break;
            case 's':
                out.story = optarg;
                break;
            case 'A':
                out.adminBin = optarg;
                break;
            case 'K':
                out.clientConf = optarg;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return false;
        }
    }
    return true;
}

// StoryId derivation matches ChronoVisor::Chronicle::getStoryId.
inline uint64_t expected_story_id(std::string const& chronicle, std::string const& story)
{
    std::string s = chronicle + story;
    return CityHash64(s.c_str(), s.length());
}

inline std::vector<std::string> read_reference_lines(std::string const& path)
{
    std::vector<std::string> out;
    std::ifstream f(path);
    std::string line;
    while(std::getline(f, line))
    {
        if(!line.empty())
            out.push_back(line);
    }
    return out;
}

// ---------------------------------------------------------------------------
// CSV lens
// ---------------------------------------------------------------------------
//
// Each line: "event : <storyId>:<eventTime>:<clientId>:<eventIndex>:<logRecord>"
// (logRecord may itself contain ':' so we split at most into a fixed number of
// fields and re-join the tail.)
inline bool parse_csv_line(std::string const& line, Event& ev)
{
    constexpr const char* kPrefix = "event : ";
    constexpr size_t kPrefixLen = 8;
    if(line.compare(0, kPrefixLen, kPrefix) != 0)
        return false;

    std::string body = line.substr(kPrefixLen);
    std::array<size_t, 4> colons{};
    size_t found = 0;
    for(size_t i = 0; i < body.size() && found < 4; ++i)
    {
        if(body[i] == ':')
            colons[found++] = i;
    }
    if(found < 4)
        return false;

    try
    {
        ev.storyId = std::stoull(body.substr(0, colons[0]));
        ev.eventTime = std::stoull(body.substr(colons[0] + 1, colons[1] - colons[0] - 1));
        ev.clientId = std::stoull(body.substr(colons[1] + 1, colons[2] - colons[1] - 1));
        ev.eventIndex = static_cast<uint32_t>(std::stoul(body.substr(colons[2] + 1, colons[3] - colons[2] - 1)));
    }
    catch(std::exception const&)
    {
        return false;
    }
    ev.logRecord = body.substr(colons[3] + 1);
    return true;
}

// CSV file naming: {chronicle}.{story}.{startSec}.{endSec}.csv
inline std::vector<std::filesystem::path>
csv_files_for_story(std::string const& csvDir, std::string const& chronicle, std::string const& story)
{
    std::vector<std::filesystem::path> out;
    std::string prefix = chronicle + "." + story + ".";
    if(!std::filesystem::is_directory(csvDir))
        return out;
    for(auto const& entry: std::filesystem::directory_iterator(csvDir))
    {
        if(!entry.is_regular_file())
            continue;
        auto name = entry.path().filename().string();
        if(name.size() > 4 && name.compare(name.size() - 4, 4, ".csv") == 0 &&
           name.compare(0, prefix.size(), prefix) == 0)
        {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

inline std::vector<Event>
collect_events_csv(std::string const& csvDir, std::string const& chronicle, std::string const& story)
{
    std::vector<Event> events;
    for(auto const& path: csv_files_for_story(csvDir, chronicle, story))
    {
        std::ifstream f(path);
        std::string line;
        while(std::getline(f, line))
        {
            Event ev;
            if(parse_csv_line(line, ev))
                events.push_back(std::move(ev));
        }
    }
    std::sort(events.begin(), events.end());
    return events;
}

// ---------------------------------------------------------------------------
// HDF5 lens (uses ChronoStore::StoryReader directly)
// ---------------------------------------------------------------------------
inline std::vector<Event>
collect_events_hdf5(std::string const& hdf5Dir, std::string const& chronicle, std::string const& story)
{
    std::vector<Event> events;
    std::string root = hdf5Dir;
    if(!root.empty() && root.back() != '/')
        root.push_back('/');
    StoryReader reader(root);

    std::string story_file = root + chronicle + "/" + story + ".h5";
    if(!std::filesystem::exists(story_file))
        return events;

    std::map<uint64_t, chronolog::StoryChunk> chunks;
    if(reader.readStory(chronicle, story_file, chunks) != 0)
        return events;

    for(auto const& [start, chunk]: chunks)
    {
        for(auto it = chunk.begin(); it != chunk.end(); ++it)
        {
            chronolog::LogEvent const& src = it->second;
            Event ev;
            ev.storyId = src.getStoryId();
            ev.eventTime = src.time();
            ev.clientId = src.getClientId();
            ev.eventIndex = src.index();
            ev.logRecord = src.getRecord();
            events.push_back(std::move(ev));
        }
    }
    std::sort(events.begin(), events.end());
    return events;
}

// ---------------------------------------------------------------------------
// Replay lens (drives chrono-client-admin -i and parses stdout)
// ---------------------------------------------------------------------------
//
// chrono-client-admin's interactive replay command prints each event as
//   {Event: <time>:<client>:<index>:<record>}
// We send '-r <chronicle> <story> 0 <UINT64_MAX>' followed by '-disconnect',
// then parse those lines.
inline std::vector<Event> collect_events_replay(std::string const& adminBin,
                                                std::string const& clientConf,
                                                std::string const& chronicle,
                                                std::string const& story)
{
    std::vector<Event> events;
    if(adminBin.empty())
        return events;

    // Build the command. Pipe the script via /bin/sh -c so we control stdin.
    std::ostringstream cmd;
    cmd << "printf '%s\\n%s\\n' " << "'-r " << chronicle << " " << story << " 0 18446744073709551615' "
        << "'-disconnect' " << "| \"" << adminBin << "\" -i";
    if(!clientConf.empty())
        cmd << " -c \"" << clientConf << "\"";
    cmd << " 2>/dev/null";

    FILE* pipe = popen(cmd.str().c_str(), "r");
    if(!pipe)
        return events;

    static const std::regex re(R"(\{Event:\s*(\d+):(\d+):(\d+):(.*)\})");
    char buf[8192];
    while(fgets(buf, sizeof(buf), pipe))
    {
        std::string line(buf);
        std::smatch m;
        if(std::regex_search(line, m, re))
        {
            Event ev;
            ev.storyId = expected_story_id(chronicle, story);
            ev.eventTime = std::stoull(m[1].str());
            ev.clientId = std::stoull(m[2].str());
            ev.eventIndex = static_cast<uint32_t>(std::stoul(m[3].str()));
            ev.logRecord = m[4].str();
            // Trim trailing CR/LF that may have leaked into the record.
            while(!ev.logRecord.empty() && (ev.logRecord.back() == '\n' || ev.logRecord.back() == '\r'))
            {
                ev.logRecord.pop_back();
            }
            events.push_back(std::move(ev));
        }
    }
    pclose(pipe);
    std::sort(events.begin(), events.end());
    return events;
}

inline std::vector<Event> collect_events(Args const& args)
{
    switch(args.lens)
    {
        case Lens::CSV:
            return collect_events_csv(args.csvDir, args.chronicle, args.story);
        case Lens::HDF5:
            return collect_events_hdf5(args.hdf5Dir, args.chronicle, args.story);
        case Lens::REPLAY:
            return collect_events_replay(args.adminBin, args.clientConf, args.chronicle, args.story);
    }
    return {};
}

inline int skip(std::string const& msg)
{
    std::cout << "[SKIP] " << msg << std::endl;
    return 0;
}

inline int pass(std::string const& msg)
{
    std::cout << "[PASS] " << msg << std::endl;
    return 0;
}

inline int fail(std::string const& msg)
{
    std::cerr << "[FAIL] " << msg << std::endl;
    return 1;
}

} // namespace data_integrity
