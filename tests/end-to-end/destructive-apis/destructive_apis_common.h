// Shared helpers for end-to-end destructive-API tests (issue #574).
//
// Tests assume a ChronoLog stack is already running (the CI's Deploy step
// or a developer's deploy_local.sh --start). Each test drives the running
// stack through chronolog::Client and then inspects the Grapher's HDF5
// archive directory to verify destructive operations actually delete files.

#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <getopt.h>

#include <ClientConfiguration.h>
#include <chronolog_client.h>

namespace destructive_apis
{

struct Args
{
    std::string clientConf;
    std::string hdf5Dir;
};

inline void usage(char const* argv0)
{
    std::cerr << "Usage: " << argv0 << " --client-conf <file> [--hdf5-dir <dir>]\n";
}

// Fill missing hdf5Dir from $CHRONOLOG_INSTALL_DIR at runtime, falling back
// to $HOME/chronolog-install/chronolog/output (the deploy_local.sh default).
// ctest doesn't export CHRONOLOG_INSTALL_DIR, so the HOME fallback is what
// most local runs and the CI's bundled deployment hit.
inline void fill_install_defaults(Args& args)
{
    if(!args.hdf5Dir.empty())
        return;
    std::string root;
    if(char const* install = std::getenv("CHRONOLOG_INSTALL_DIR"); install != nullptr && *install != '\0')
    {
        root = install;
    }
    else if(char const* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        root = std::string(home) + "/chronolog-install/chronolog";
    }
    if(!root.empty())
    {
        args.hdf5Dir = root + "/output";
    }
}

inline bool parse_args(int argc, char** argv, Args& out)
{
    static option longopts[] = {{"client-conf", required_argument, nullptr, 'c'},
                                {"hdf5-dir", required_argument, nullptr, 'H'},
                                {"help", no_argument, nullptr, 'h'},
                                {nullptr, 0, nullptr, 0}};
    int opt;
    while((opt = getopt_long(argc, argv, "c:H:h", longopts, nullptr)) != -1)
    {
        switch(opt)
        {
            case 'c':
                out.clientConf = optarg;
                break;
            case 'H':
                out.hdf5Dir = optarg;
                break;
            case 'h':
            default:
                usage(argv[0]);
                return false;
        }
    }
    fill_install_defaults(out);
    if(out.clientConf.empty())
    {
        std::cerr << "Missing required --client-conf\n";
        usage(argv[0]);
        return false;
    }
    return true;
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

// Count HDF5 chunk files the Grapher would have written for a story:
//   <hdf5Dir>/<chronicle>.<story>.<startSec>.vlen.h5
// (plus the rotated <...>.<n>.vlen.h5 form). Returns 0 when the directory
// doesn't exist (an empty deployment hasn't created it yet).
inline size_t count_story_files(std::string const& hdf5Dir, std::string const& chronicle, std::string const& story)
{
    if(!std::filesystem::is_directory(hdf5Dir))
        return 0;
    std::string const prefix = chronicle + "." + story + ".";
    size_t n = 0;
    for(auto const& entry: std::filesystem::directory_iterator(hdf5Dir))
    {
        if(!entry.is_regular_file())
            continue;
        auto name = entry.path().filename().string();
        if(name.size() < 3 || name.compare(name.size() - 3, 3, ".h5") != 0)
            continue;
        if(name.compare(0, prefix.size(), prefix) != 0)
            continue;
        ++n;
    }
    return n;
}

inline size_t count_chronicle_files(std::string const& hdf5Dir, std::string const& chronicle)
{
    if(!std::filesystem::is_directory(hdf5Dir))
        return 0;
    std::string const prefix = chronicle + ".";
    size_t n = 0;
    for(auto const& entry: std::filesystem::directory_iterator(hdf5Dir))
    {
        if(!entry.is_regular_file())
            continue;
        auto name = entry.path().filename().string();
        if(name.size() < 3 || name.compare(name.size() - 3, 3, ".h5") != 0)
            continue;
        if(name.compare(0, prefix.size(), prefix) != 0)
            continue;
        ++n;
    }
    return n;
}

// Generate a unique chronicle/story name suffix so tests can be re-run
// against a long-lived deployment without colliding with prior runs.
inline std::string unique_suffix()
{
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<uint64_t>(now));
}

inline chronolog::ClientPortalServiceConf load_portal_conf(std::string const& path)
{
    chronolog::ClientConfiguration cfg;
    if(!cfg.load_from_file(path))
    {
        std::cerr << "[destructive-apis] failed to load client conf " << path << std::endl;
    }
    return cfg.PORTAL_CONF;
}

inline chronolog::ClientQueryServiceConf load_query_conf(std::string const& path)
{
    chronolog::ClientConfiguration cfg;
    if(!cfg.load_from_file(path))
    {
        std::cerr << "[destructive-apis] failed to load client conf " << path << std::endl;
    }
    return cfg.QUERY_CONF;
}

// Generate `count` log records, padded so a story chunk fills out fast.
// Returns the number of events successfully logged.
inline size_t write_events(chronolog::StoryHandle* handle, size_t count, std::string const& tag)
{
    size_t written = 0;
    for(size_t i = 0; i < count; ++i)
    {
        std::ostringstream os;
        os << "[" << tag << "] event " << i;
        // log_event returns the event timestamp (non-zero) on success and 0
        // on failure (a recording client wasn't selected for this story).
        if(handle->log_event(os.str()) != 0)
            ++written;
    }
    return written;
}

} // namespace destructive_apis
