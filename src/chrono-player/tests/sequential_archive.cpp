#define CHRONOLOG_SEQUENTIAL_ARCHIVE_READS
#define HotReplay SequentialHotReplay
#include "chrono-player/replay/HotReplay.cpp"
#undef HotReplay

namespace chronolog::player
{
std::unique_ptr<Replay> sequentialArchive(std::shared_ptr<const HotSource> source, HotReplayOptions options)
{
    return std::make_unique<SequentialHotReplay>(std::move(source), std::move(options));
}
} // namespace chronolog::player
