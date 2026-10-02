// Microbenchmarks without services: codec cost, CRC32C, HLC assignment and WAL group commit.
// Run through tools/bench/run.sh, which adds the metadata record. No result is asserted.
#include <benchmark/benchmark.h>

#include <absl/crc/crc32c.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "codec.h"
#include "clock/HlcCore.h"
#include "wal/FileSink.h"

namespace
{
using namespace chronolog;
using namespace chronolog::bench;

constexpr size_t maxMessageBytes = 16u << 20;

void codecArgs(benchmark::Benchmark* b)
{
    for(int message = 0; message < 3; ++message)
        for(int payload: {64, 1024, 16384, 262144})
            for(int batch: {1, 16, 256})
                for(int arena: {0, 1})
                    if(static_cast<size_t>(payload) * static_cast<size_t>(batch) <= maxMessageBytes)
                        b->Args({message, payload, batch, arena});
}

std::unique_ptr<CodecCase> prepare(benchmark::State& state, const Codec& codec)
{
    const auto message = static_cast<Message>(state.range(0));
    const auto workload =
            makeWorkload(message, static_cast<size_t>(state.range(1)), static_cast<size_t>(state.range(2)));
    state.SetLabel(std::string(codec.name()) + "/" + messageName(message) + (state.range(3) ? "/arena" : "/heap"));
    return codec.prepare(workload, state.range(3) != 0);
}

void report(benchmark::State& state, size_t wire_bytes)
{
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * wire_bytes));
    state.SetItemsProcessed(state.iterations() * state.range(2));
    state.counters["wire_bytes"] = static_cast<double>(wire_bytes);
}

void encode(benchmark::State& state)
{
    const auto all = codecs();
    auto c = prepare(state, *all.front());
    for(auto _: state) benchmark::DoNotOptimize(c->encode());
    report(state, c->wireBytes());
}

void decode(benchmark::State& state)
{
    const auto all = codecs();
    auto c = prepare(state, *all.front());
    for(auto _: state)
        if(!c->decode())
        {
            state.SkipWithError("decode failed");
            break;
        }
    report(state, c->wireBytes());
}
BENCHMARK(encode)->Apply(codecArgs);
BENCHMARK(decode)->Apply(codecArgs);

void crc32c(benchmark::State& state)
{
    const std::string bytes(static_cast<size_t>(state.range(0)), 'x');
    for(auto _: state) benchmark::DoNotOptimize(absl::ComputeCrc32c(bytes));
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
}
BENCHMARK(crc32c)->Arg(64)->Arg(1024)->Arg(16384)->Arg(262144)->Arg(4 << 20);

int64_t realtimeNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

HlcCore& sharedClock()
{
    static HlcCore core;
    return core;
}

void hlcTick(benchmark::State& state)
{
    for(auto _: state) benchmark::DoNotOptimize(sharedClock().tick(realtimeNs()));
}
void hlcAssignChecked(benchmark::State& state)
{
    for(auto _: state)
    {
        const auto now = realtimeNs();
        auto result = sharedClock().assignChecked(now, Hlc{}, PhysicalInterval{now, now, true});
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(hlcTick)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();
BENCHMARK(hlcAssignChecked)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();

// One group commit is a single write of group_size records followed by one fdatasync, as the WAL does.
void walGroupCommit(benchmark::State& state)
{
    const size_t group = static_cast<size_t>(state.range(0)), record = static_cast<size_t>(state.range(1));
    const char* dir = std::getenv("CHRONOLOG_BENCH_WAL_DIR");
    const auto folder =
            std::filesystem::path(dir ? dir : "/tmp") / ("chronolog-bench-wal-" + std::to_string(::getpid()));
    std::filesystem::create_directories(folder);
    const auto path = (folder / "wal.log").string();
    {
        auto sink = openFileSink(path);
        const std::string bytes(group * record, 'w');
        for(auto _: state)
        {
            if(!sink->write(bytes).ok() || !sink->sync().ok())
            {
                state.SkipWithError("WAL write or sync failed");
                break;
            }
        }
        state.SetBytesProcessed(static_cast<int64_t>(state.iterations() * bytes.size()));
        state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * group));
    }
    std::filesystem::remove_all(folder);
}
BENCHMARK(walGroupCommit)->ArgsProduct({{1, 4, 16, 64, 256}, {64, 1024}})->UseRealTime();
} // namespace

BENCHMARK_MAIN();
