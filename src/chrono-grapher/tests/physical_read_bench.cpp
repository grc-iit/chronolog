#include "chrono-grapher/tier/FileTierStore.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
using namespace chronolog;
constexpr size_t Files = 1000, EventsPerFile = 64, Target = Files / 2;

int Create(const std::filesystem::path& root)
{
    auto store = FileTierStore::Open(root, "physical-bench", {{1, {100, 0}}});
    if(!store.ok())
    {
        std::cerr << store.status() << '\n';
        return 1;
    }
    for(size_t file = 0; file < Files; ++file)
    {
        const auto start = static_cast<int64_t>(100 + file * 1000);
        Chunk chunk{"physical-" + std::to_string(file), 1, {start, 0}, {start + 1000, 0}, {}};
        chunk.physical_policy = true;
        for(size_t i = 0; i < EventsPerFile; ++i)
        {
            Event event;
            event.id = {1, 2, 3, file * EventsPerFile + i + 1};
            event.hlc = {start + static_cast<int64_t>(i), 0};
            event.physical = {static_cast<int64_t>(file * 1'000'000 + i * 10), 2, ClockStatus::Synced};
            event.envelope = {"application/octet-stream", std::string(1024, 'x'), {}, {}, {}};
            chunk.events.push_back(std::move(event));
        }
        auto published = (*store)->publish(std::move(chunk));
        if(!published.ok())
        {
            std::cerr << published.status() << '\n';
            return 1;
        }
    }
    return 0;
}

int Read(const std::filesystem::path& root)
{
    std::atomic<size_t> opened{};
    auto store = FileTierStore::OpenReadOnly(root,
                                             std::chrono::hours(1),
                                             [&](const auto& path)
                                             {
                                                 ++opened;
                                                 return ReadChunkFile(path);
                                             });
    if(!store.ok())
    {
        std::cerr << store.status() << '\n';
        return 1;
    }
    auto manifest = (*store)->manifest(1);
    if(!manifest.ok() || manifest->size() != Files)
        return 1;
    const auto start = static_cast<int64_t>(Target * 1'000'000);
    const Range range{Range::Axis::Physical, {start, 0}, {start + 1000, 0}};
    for(const bool direct: {false, true})
        for(size_t run = 0; run < 3; ++run)
        {
            opened = 0;
            const auto began = std::chrono::steady_clock::now();
            std::vector<Event> result;
            if(direct)
            {
                for(const auto& record: *manifest)
                {
                    auto events = (*store)->readRecord(record, range);
                    if(!events.ok())
                    {
                        std::cerr << events.status() << '\n';
                        return 1;
                    }
                    result.insert(result.end(), events->begin(), events->end());
                }
            }
            else
            {
                auto events = (*store)->read(1, range);
                if(!events.ok())
                {
                    std::cerr << events.status() << '\n';
                    return 1;
                }
                result = *std::move(events);
            }
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            if(result.size() != EventsPerFile)
                return 1;
            for(const auto& event: result)
                if(event.id.sequence <= Target * EventsPerFile || event.id.sequence > (Target + 1) * EventsPerFile)
                    return 1;
            std::cout << nlohmann::json{{"api", direct ? "readRecord" : "read"},
                                        {"run", run},
                                        {"archive_files", Files},
                                        {"files_opened", opened.load()},
                                        {"events", result.size()},
                                        {"wall_seconds", elapsed}}
                                 .dump()
                      << '\n';
        }
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    const std::string mode = argv[1];
    if(mode == "create")
        return Create(argv[2]);
    if(mode == "read")
        return Read(argv[2]);
    return 2;
}
