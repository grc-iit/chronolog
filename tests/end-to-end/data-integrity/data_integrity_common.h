// Shared helpers for end-to-end data-integrity tests.
//
// Three property tests (StoryID consistency, event count, event order)
// validate that events injected via chrono-client-admin -f land in the
// on-disk HDF5 archive written by ChronoGrapher (files named
// "{chronicle}.{story}.<timestamp>.vlen.h5", flat in the deployment's
// output dir).
//
// A Replay lens (chrono-client-admin -i + '-r ...') and a CSV lens were
// both explored earlier and removed from this iteration:
//   - Replay: the read-path is out of scope for this first integrity-test
//     landing — to be wired up in a follow-up issue.
//   - CSV: the active pipeline no longer produces CSVs (ChronoKeeper uses
//     LoggingExtractor + RDMATransferAgent, ChronoGrapher writes HDF5).
// The HDF5 archive is the authoritative durable store, so checking it
// directly is sufficient to answer "did the data we wrote reach the end?"

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <getopt.h>

#include <H5Cpp.h>

#include "city.h"

namespace data_integrity
{

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
    std::string hdf5Dir;
    std::string referenceFile;
    std::string chronicle = "chronicle_0_0";
    std::string story = "story_0_0";
};

inline void usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0
              << " --reference <file> [--hdf5-dir <dir>]"
                 " [--chronicle <name>] [--story <name>]\n";
}

// Fill missing hdf5Dir from $CHRONOLOG_INSTALL_DIR at runtime, falling back
// to $HOME/chronolog-install/chronolog (the deploy_local.sh default and the
// same fallback the fixture script uses). ctest doesn't export
// CHRONOLOG_INSTALL_DIR by default, so the HOME fallback matters.
inline void fill_install_defaults(Args& args)
{
    if(!args.hdf5Dir.empty())
        return;
    std::string root;
    if(const char* install = std::getenv("CHRONOLOG_INSTALL_DIR"); install != nullptr && *install != '\0')
    {
        root = install;
    }
    else if(const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        root = std::string(home) + "/chronolog-install/chronolog";
    }
    else
    {
        return;
    }
    args.hdf5Dir = root + "/output";
}

inline bool parse_args(int argc, char** argv, Args& out)
{
    static option longopts[] = {{"hdf5-dir", required_argument, nullptr, 'H'},
                                {"reference", required_argument, nullptr, 'R'},
                                {"chronicle", required_argument, nullptr, 'c'},
                                {"story", required_argument, nullptr, 's'},
                                {"help", no_argument, nullptr, 'h'},
                                {nullptr, 0, nullptr, 0}};
    int opt;
    while((opt = getopt_long(argc, argv, "H:R:c:s:h", longopts, nullptr)) != -1)
    {
        switch(opt)
        {
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
            case 'h':
            default:
                usage(argv[0]);
                return false;
        }
    }
    fill_install_defaults(out);
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

// ChronoGrapher writes archive files flat in its configured story_files_dir,
// named "{chronicle}.{story}.<timestamp>.vlen.h5" (one file per flushed
// chunk). We scan the output dir for anything matching that prefix and read
// each matching file as a Story via its compound dataset.
inline std::vector<std::filesystem::path>
hdf5_files_for_story(std::string const& hdf5Dir, std::string const& chronicle, std::string const& story)
{
    std::vector<std::filesystem::path> out;
    std::string prefix = chronicle + "." + story + ".";
    if(!std::filesystem::is_directory(hdf5Dir))
        return out;
    for(auto const& entry: std::filesystem::directory_iterator(hdf5Dir))
    {
        if(!entry.is_regular_file())
            continue;
        auto name = entry.path().filename().string();
        if(name.size() < 3 || name.compare(name.size() - 3, 3, ".h5") != 0)
            continue;
        if(name.compare(0, prefix.size(), prefix) != 0)
            continue;
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// POD with the same layout as chronolog::LogEventHVL (data members only) but
// without an owning destructor. Letting LogEventHVL's destructor run after
// H5::DataSet::vlenReclaim caused a double free; this POD avoids it.
struct LogEventHVLPOD
{
    uint64_t storyId;
    uint64_t eventTime;
    uint32_t clientId;
    uint32_t eventIndex;
    hvl_t logRecord;
};

// Read all events from one chunk file written by StoryChunkWriter:
//   /story_chunks/data.vlen_bytes  – compound dataset of LogEventHVL
inline void read_events_from_chunk_file(std::filesystem::path const& path, std::vector<Event>& out)
{
    try
    {
        H5::H5File file(path.string(), H5F_ACC_RDONLY);
        H5::DataSet dataset = file.openDataSet("/story_chunks/data.vlen_bytes");
        H5::DataSpace space = dataset.getSpace();
        hsize_t n = 0;
        space.getSimpleExtentDims(&n, nullptr);
        if(n == 0)
            return;

        H5::CompType type(sizeof(LogEventHVLPOD));
        type.insertMember("storyId", HOFFSET(LogEventHVLPOD, storyId), H5::PredType::NATIVE_UINT64);
        type.insertMember("eventTime", HOFFSET(LogEventHVLPOD, eventTime), H5::PredType::NATIVE_UINT64);
        type.insertMember("clientId", HOFFSET(LogEventHVLPOD, clientId), H5::PredType::NATIVE_UINT32);
        type.insertMember("eventIndex", HOFFSET(LogEventHVLPOD, eventIndex), H5::PredType::NATIVE_UINT32);
        type.insertMember("logRecord", HOFFSET(LogEventHVLPOD, logRecord), H5::VarLenType(H5::PredType::NATIVE_UINT8));

        std::vector<LogEventHVLPOD> raw(n);
        dataset.read(raw.data(), type);

        for(auto const& src: raw)
        {
            Event ev;
            ev.storyId = src.storyId;
            ev.eventTime = src.eventTime;
            ev.clientId = src.clientId;
            ev.eventIndex = src.eventIndex;
            if(src.logRecord.p && src.logRecord.len > 0)
            {
                ev.logRecord.assign(static_cast<char const*>(src.logRecord.p), src.logRecord.len);
            }
            out.push_back(std::move(ev));
        }

        // Reclaim the variable-length buffers HDF5 allocated during read.
        H5::DataSet::vlenReclaim(raw.data(), type, space);
    }
    catch(H5::Exception const& e)
    {
        std::cerr << "[hdf5] failed to read " << path << ": " << e.getCDetailMsg() << "\n";
    }
}

inline std::vector<Event> collect_events(Args const& args)
{
    std::vector<Event> events;
    for(auto const& path: hdf5_files_for_story(args.hdf5Dir, args.chronicle, args.story))
    {
        read_events_from_chunk_file(path, events);
    }
    std::sort(events.begin(), events.end());
    return events;
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
