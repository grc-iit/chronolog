// Offline verification uses the production manifest/HDF5 reader. It never
// constructs a client, Margo engine, native runtime, thread, or synthetic data.
#include <HDF5ArchiveReadingAgent.h>
#include <abt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <tuple>
#include <string>

namespace
{
using Identity = std::tuple<uint64_t, uint64_t, uint32_t>;
std::string hex(std::string const& value)
{
    static char const digits[] = "0123456789abcdef";
    std::string result;
    for(unsigned char byte: value)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
size_t threads()
{
    size_t count = 0;
    for(auto const& entry: std::filesystem::directory_iterator("/proc/self/task")) ++count;
    return count;
}
} // namespace
int main(int argc, char** argv)
{
    if(argc != 5)
        return 2;
    bool const before = ABT_initialized() == ABT_ERR_UNINITIALIZED && threads() == 1;
    std::map<Identity, std::string> expected;
    std::ifstream input(argv[4]);
    std::string tag, payload;
    uint64_t timestamp, client;
    uint32_t index;
    uint64_t first = UINT64_MAX, last = 0;
    while(input >> tag >> timestamp >> client >> index >> payload)
    {
        if(tag != "EVENT_ID" || !expected.emplace(Identity{timestamp, client, index}, payload).second)
            return 3;
        first = std::min(first, timestamp);
        last = std::max(last, timestamp);
    }
    if(expected.size() != 8)
        return 3;
    chronolog::HDF5ArchiveReadingAgent reader(argv[1]);
    if(reader.initialize() != chronolog::CL_SUCCESS)
        return 4;
    std::list<chronolog::StoryChunk*> chunks;
    int const status = reader.readArchivedStory(argv[2], argv[3], first, last + 1, chunks);
    std::map<Identity, std::string> actual;
    size_t count = 0;
    for(auto chunk: chunks)
    {
        for(auto const& item: *chunk)
        {
            auto const& event = item.second;
            ++count;
            actual.emplace(Identity{event.time(), event.getClientId(), event.index()}, hex(event.getRecord()));
        }
        delete chunk;
    }
    bool const after = ABT_initialized() == ABT_ERR_UNINITIALIZED && threads() == 1;
    bool const exact = status == chronolog::CL_SUCCESS && count == expected.size() && actual == expected;
    std::cout << "ARCHIVE " << status << " " << count << " " << exact << " " << before << " " << after << std::endl;
    return exact && before && after ? 0 : 5;
}
