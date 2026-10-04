#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "visor/membership/Topology.h"

namespace chronolog::visor::testing
{

inline Topology twoKeeperTopology()
{
    return Topology{{{"keeper-a", "keeper-a:50052"}, {"keeper-b", "keeper-b:50052"}}, "grapher:50053", "player:50054"};
}

// A private temporary directory removed on destruction.
class TempDir
{
public:
    TempDir()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "chronolog-visor-XXXXXX").string();
        path_ = ::mkdtemp(pattern.data());
    }
    ~TempDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace chronolog::visor::testing
