#include "tier/FileTierStore.h"
#include <chrono>
#include <iostream>

// Prints, never asserts (RFC-I 2.6 (d), I13.17): seeds an archive with one writer's chunk files, times
// FileTierStore::Open over it, and runs one unpaced scrubber pass that leaves the validated mark.
namespace
{
using namespace chronolog;
using Clock = std::chrono::steady_clock;
constexpr size_t EventsPerFile = 8;
constexpr int64_t Base = 1'700'000'000'000'000'000, Window = 30'000'000'000;

Chunk Make(size_t index)
{
    const int64_t start = Base + static_cast<int64_t>(index) * Window;
    Chunk chunk{"bench-" + std::to_string(index), 1, {start, 0}, {start + Window, 0}, {}};
    for(size_t i = 0; i < EventsPerFile; ++i)
    {
        Event event;
        event.id = {1, 2, 3, index * EventsPerFile + i + 1};
        event.hlc = {start + static_cast<int64_t>(i) * 1000, 0};
        event.physical = {start + static_cast<int64_t>(i) * 1000, 2000, ClockStatus::Synced};
        event.durability = Durability::Durable;
        event.envelope = {"application/octet-stream",
                          std::string(1024, static_cast<char>('a' + i)),
                          std::string(16, 't'),
                          std::string(8, 's'),
                          {{"host", "bench"}, {"rank", std::to_string(i)}}};
        chunk.events.push_back(std::move(event));
    }
    return chunk;
}

double Ms(Clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }
} // namespace

int main(int argc, char** argv)
{
    if(argc < 4)
    {
        std::cerr << "usage: chronolog_archive_recovery_bench seed|open|scrub <archive-root> <writer> [files]\n";
        return 2;
    }
    const std::string mode = argv[1], root = argv[2], writer = argv[3];
    const auto started = Clock::now();
    auto store = FileTierStore::Open(root, writer, {{1, {Base, 0}}});
    const auto opened = Clock::now();
    if(!store.ok())
    {
        std::cerr << store.status() << '\n';
        return 1;
    }
    if(mode == "open")
    {
        const auto records = (*store)->manifest(1);
        std::cout << "open_ms=" << Ms(opened - started) << " records=" << (records.ok() ? records->size() : 0)
                  << " mark=" << (ManifestLog::ValidatedMark(root, writer) ? "yes" : "no") << '\n';
        return 0;
    }
    if(mode == "scrub")
    {
        const auto result = (*store)->scrubOnce(0);
        if(!result.ok())
        {
            std::cerr << result.status() << '\n';
            return 1;
        }
        std::cout << "scrub_ms=" << Ms(Clock::now() - opened) << " validated=" << result->validated
                  << " lost=" << result->lost << " through=" << result->through << '\n';
        return 0;
    }
    if(mode != "seed" || argc < 5)
        return 2;
    const size_t files = std::stoul(argv[4]);
    for(size_t index = 0; index < files; ++index)
        if(auto published = (*store)->publish(Make(index)); !published.ok())
        {
            std::cerr << published.status() << '\n';
            return 1;
        }
    std::cout << "seeded files=" << files << " seed_ms=" << Ms(Clock::now() - opened) << '\n';
    return 0;
}
