#include "chrono-player/replay/HotReplay.h"
#include <chrono>
#include <iostream>

namespace
{
using namespace chronolog;
using namespace chronolog::player;
constexpr size_t Files = 256, Events = 64;
constexpr Hlc End{100 + Files * 1000, 0};

class ArchiveSource final: public HotSource
{
public:
    absl::StatusOr<HotFetch> fetch(StoryId, const Range&) const override
    {
        HotFetch result;
        result.route_epoch = 1;
        result.archived_below = End;
        result.keepers = {{{"archive", 1, End, true, false, End}, {}}};
        return result;
    }
    absl::StatusOr<HotFetch> fetchTail(StoryId story, Hlc from, const TailStarts&) const override
    {
        auto result = fetch(story, {});
        result->closed = from >= End;
        return result;
    }
};

int create(const std::filesystem::path& root)
{
    auto writer = FileTierStore::Open(root, "pool-bench", {{1, {100, 0}}});
    if(!writer.ok())
    {
        std::cerr << writer.status() << '\n';
        return 1;
    }
    for(size_t file = 0; file < Files; ++file)
    {
        const auto start = static_cast<int64_t>(100 + file * 1000);
        Chunk chunk{std::to_string(file), 1, {start, 0}, {start + 1000, 0}, {}};
        for(size_t i = 0; i < Events; ++i)
        {
            Event event;
            event.id = {1, 2, 3, file * Events + i + 1};
            event.hlc = {start + static_cast<int64_t>(i), 1};
            event.physical = {event.hlc.physical_ns, 0, ClockStatus::Synced};
            event.envelope.payload = std::string(1024, 'x');
            chunk.events.push_back(std::move(event));
        }
        auto published = (*writer)->publish(std::move(chunk));
        if(!published.ok())
        {
            std::cerr << published.status() << '\n';
            return 1;
        }
    }
    return 0;
}

int measure(const std::filesystem::path& root, bool tail)
{
    auto reader = FileTierStore::OpenReadOnly(root, std::chrono::hours(1), {}, 8);
    if(!reader.ok())
        return 1;
    HotReplayOptions options;
    options.archive = std::shared_ptr<FileTierStore>(*std::move(reader));
    options.batch_size = Files * Events;
    HotReplay replay(std::make_shared<ArchiveSource>(), options);
    Event position;
    position.id.story_id = 1;
    const auto began = std::chrono::steady_clock::now();
    auto stream = tail ? replay.tail(1, position) : replay.read(1, {Range::Axis::Hlc, {100, 0}, End});
    if(!stream.ok())
    {
        std::cerr << stream.status() << '\n';
        return 1;
    }
    size_t count = 0, bytes = 0;
    bool complete = false;
    Hlc frontier;
    for(size_t round = 0; round < 100; ++round)
    {
        auto batch = (*stream)->next();
        if(!batch.ok())
        {
            std::cerr << batch.status() << '\n';
            return 1;
        }
        if(!*batch)
            break;
        count += (**batch).events.size();
        for(const auto& event: (**batch).events) bytes += event.envelope.payload.size();
        if((**batch).completion)
        {
            complete = true;
            frontier = (**batch).completion->frontier;
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    std::cout << (tail ? "Tail" : "Read") << " wall_ms=" << elapsed << " events=" << count << " bytes=" << bytes
              << " frontier=" << frontier.physical_ns << " files=" << Files << " events_per_file=" << Events
              << " payload_bytes=1024 read_threads=8\n";
    return count == Files * Events && bytes == Files * Events * 1024 && complete && frontier == End ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 3)
        return 2;
    const std::string mode = argv[1];
    if(mode == "create")
        return create(argv[2]);
    if(mode == "read" || mode == "tail")
        return measure(argv[2], mode == "tail");
    return 2;
}
