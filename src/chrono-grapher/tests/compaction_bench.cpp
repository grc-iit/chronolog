#include "chrono-grapher/tier/FileTierStore.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

// Prints, never asserts: an HLC read over a 2880-file story before and after compaction (files opened, wall time)
// and publish latency with and without a compaction job running.
namespace
{
using namespace chronolog;
using Clock = std::chrono::steady_clock;
constexpr size_t Files = 2880, EventsPerFile = 8, Samples = 5, Publishes = 300;
constexpr int64_t Base = 1'700'000'000'000'000'000, Window = 30'000'000'000;

Chunk Make(StoryId story, size_t index)
{
    const int64_t start = Base + static_cast<int64_t>(index) * Window;
    Chunk chunk{"bench-" + std::to_string(index), story, {start, 0}, {start + Window, 0}, {}};
    for(size_t i = 0; i < EventsPerFile; ++i)
    {
        Event event;
        event.id = {story, 2, 3, index * EventsPerFile + i + 1};
        event.hlc = {start + static_cast<int64_t>(i) * 1000, 0};
        event.physical = {start + static_cast<int64_t>(i) * 1000, 2000, ClockStatus::Synced};
        event.durability = Durability::Durable;
        event.envelope = {"application/octet-stream",
                          std::string(1024, static_cast<char>('a' + i)),
                          std::string(16, 't'),
                          std::string(8, 's'),
                          {{"host", "dragon"}, {"rank", std::to_string(i)}}};
        chunk.events.push_back(std::move(event));
    }
    return chunk;
}

double Ms(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }

void Summary(const char* label, std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    std::cout << label << " median_ms=" << samples[samples.size() / 2] << " min_ms=" << samples.front()
              << " max_ms=" << samples.back();
}

void Percentiles(const char* label, std::vector<double> samples)
{
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) { return samples[std::min(samples.size() - 1, size_t(q * samples.size()))]; };
    std::cout << label << " publishes=" << samples.size() << " p50_ms=" << at(0.5) << " p99_ms=" << at(0.99)
              << " max_ms=" << samples.back() << '\n';
}

// Returns the event ids read, or empty on failure.
std::vector<EventId> Read(const std::filesystem::path& root, const char* state)
{
    std::atomic<size_t> opened{};
    auto store = FileTierStore::OpenReadOnly(root,
                                             std::chrono::hours(1),
                                             [&](const auto& path)
                                             {
                                                 ++opened;
                                                 return LoadChunkFile(path);
                                             });
    if(!store.ok())
    {
        std::cerr << store.status() << '\n';
        return {};
    }
    const Range day{Range::Axis::Hlc, {Base, 0}, {Base + static_cast<int64_t>(Files) * Window, 0}};
    std::vector<double> wall;
    std::vector<EventId> ids;
    for(size_t run = 0; run < Samples; ++run)
    {
        opened = 0;
        const auto began = Clock::now();
        auto events = (*store)->read(1, day);
        wall.push_back(Ms(Clock::now() - began));
        if(!events.ok() || events->size() != Files * EventsPerFile)
        {
            std::cerr << "read failed " << (events.ok() ? absl::OkStatus() : events.status()) << '\n';
            return {};
        }
        ids.clear();
        for(const auto& event: *events) ids.push_back(event.id);
    }
    std::cout << "hlc_read state=" << state << " files=" << (*store)->manifest(1)->size() << " opened=" << opened.load()
              << " events=" << ids.size() << ' ';
    Summary("wall", wall);
    std::cout << '\n';
    return ids;
}

int Run(const std::filesystem::path& root)
{
    std::filesystem::remove_all(root);
    auto store = FileTierStore::Open(root, "bench", {{1, {Base, 0}}, {2, {Base, 0}}});
    if(!store.ok())
    {
        std::cerr << store.status() << '\n';
        return 1;
    }
    const auto created = Clock::now();
    for(size_t file = 0; file < Files; ++file)
        if(auto published = (*store)->publish(Make(1, file)); !published.ok())
        {
            std::cerr << published.status() << '\n';
            return 1;
        }
    std::cout << "created files=" << Files << " events=" << Files * EventsPerFile
              << " ms=" << Ms(Clock::now() - created) << '\n';
    const auto before = Read(root, "before");
    const auto w = (*store)->contiguousWatermark(1);

    CompactionPolicy policy;
    policy.min_age = std::chrono::seconds(0);
    std::vector<double> idle, busy;
    size_t next = 0;
    for(size_t i = 0; i < Publishes; ++i)
    {
        const auto began = Clock::now();
        if(!(*store)->publish(Make(2, next++)).ok())
            return 1;
        idle.push_back(Ms(Clock::now() - began));
    }
    std::atomic<bool> done{};
    std::atomic<size_t> inputs{}, outputs{};
    const auto compaction_began = Clock::now();
    Clock::duration compaction_took{};
    std::jthread compactor(
            [&]
            {
                while(true)
                {
                    auto result = (*store)->compactOnce(policy);
                    if(!result.ok())
                        std::cerr << "compaction: " << result.status() << '\n';
                    if(!result.ok() || !result->inputs)
                        break;
                    inputs += result->inputs;
                    ++outputs;
                }
                compaction_took = Clock::now() - compaction_began;
                done = true;
            });
    // Publishes go to story 2 while the job works through story 1's backlog.
    while(!done && busy.size() < Publishes)
    {
        const auto began = Clock::now();
        if(!(*store)->publish(Make(2, next++)).ok())
            return 1;
        busy.push_back(Ms(Clock::now() - began));
    }
    const size_t inputs_during_publish = inputs.load();
    compactor.join();
    std::cout << "compaction inputs=" << inputs.load() << " outputs=" << outputs.load() << " ms=" << Ms(compaction_took)
              << " inputs_while_publishing=" << inputs_during_publish << " io_bytes_per_sec=" << policy.io_bytes_per_sec
              << '\n';
    Percentiles("publish compaction=idle", idle);
    Percentiles("publish compaction=running", busy);
    const auto after = Read(root, "after");
    std::cout << "equal_events=" << (!before.empty() && before == after)
              << " watermark_equal=" << (w.ok() && (*store)->contiguousWatermark(1).value() == *w) << '\n';
    return before.empty() || before != after ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
    if(argc != 2)
    {
        std::cerr << "usage: chronolog_archive_compaction_bench <empty-directory>\n";
        return 2;
    }
    return Run(argv[1]);
}
