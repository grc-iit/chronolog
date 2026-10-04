#include "common/tier/ChunkCodec.h"
#include "player/replay/HotReplay.h"
#include <atomic>
#include <chrono>
#include <iostream>

// Prints, never asserts: one cold Player HLC read over a 600-file story before and after compaction (files loaded,
// wall time).
namespace
{
using namespace chronolog;
using namespace chronolog::player;
constexpr size_t Files = 600, Events = 8;
constexpr int64_t Window = 1000;
constexpr Hlc End{100 + static_cast<int64_t>(Files) * Window, 0};

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
};

bool measure(const std::filesystem::path& root, const char* state)
{
    std::atomic<size_t> loaded{};
    auto reader = FileTierStore::OpenReadOnly(
            root,
            std::chrono::hours(1),
            [&](const std::filesystem::path& file)
            {
                ++loaded;
                return LoadChunkFile(file);
            },
            8);
    if(!reader.ok())
        return false;
    HotReplayOptions options;
    options.archive = std::shared_ptr<FileTierStore>(*std::move(reader));
    options.batch_size = Files * Events;
    HotReplay replay(std::make_shared<ArchiveSource>(), options);
    const auto began = std::chrono::steady_clock::now();
    auto stream = replay.read(1, {Range::Axis::Hlc, {100, 0}, End});
    if(!stream.ok())
        return false;
    size_t count = 0;
    bool complete = false;
    for(size_t round = 0; round < 100; ++round)
    {
        auto batch = (*stream)->next();
        if(!batch.ok() || !*batch)
            break;
        count += (**batch).events.size();
        if((**batch).completion)
            complete = (**batch).completion->complete;
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    auto manifest = options.archive->manifest(1);
    std::cout << state << " records=" << (manifest.ok() ? manifest->size() : 0) << " files_loaded=" << loaded
              << " wall_ms=" << elapsed << " events=" << count << " complete=" << complete << '\n';
    return count == Files * Events && complete;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 2)
        return 2;
    const std::filesystem::path root = argv[1];
    std::filesystem::remove_all(root);
    auto writer = FileTierStore::Open(root, "read-bench", {{1, {100, 0}}});
    if(!writer.ok())
        return 1;
    for(size_t file = 0; file < Files; ++file)
    {
        const auto start = static_cast<int64_t>(100 + file * Window);
        Chunk chunk{std::to_string(file), 1, {start, 0}, {start + Window, 0}, {}};
        for(size_t i = 0; i < Events; ++i)
        {
            Event event;
            event.id = {1, 2, 3, file * Events + i + 1};
            event.hlc = {start + static_cast<int64_t>(i), 1};
            event.physical = {event.hlc.physical_ns, 0, ClockStatus::Synced};
            event.envelope.payload = std::string(1024, 'x');
            chunk.events.push_back(std::move(event));
        }
        if(!(*writer)->publish(std::move(chunk)).ok())
            return 1;
    }
    if(!measure(root, "before"))
        return 1;
    CompactionPolicy policy;
    policy.min_age = std::chrono::seconds(0);
    policy.io_bytes_per_sec = policy.io_burst_bytes = uint64_t{1} << 30;
    size_t jobs = 0;
    for(;;)
    {
        auto result = (*writer)->compactOnce(policy);
        if(!result.ok())
            return 1;
        if(!result->inputs)
            break;
        ++jobs;
    }
    std::cout << "compaction_jobs=" << jobs << '\n';
    const bool ok = measure(root, "after");
    writer->reset();
    std::filesystem::remove_all(root);
    return ok ? 0 : 1;
}
