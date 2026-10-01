#pragma once

#include <chrono>
#include <memory>
#include "chrono-player/replay/HotSource.h"
#include "chrono-grapher/tier/FileTierStore.h"
#include "chronolog/replay.h"

namespace chronolog::player
{

struct HotReplayOptions
{
    size_t batch_size{1024};
    std::chrono::milliseconds tail_poll{200};
    std::shared_ptr<const FileTierStore> archive;
};

class HotReplay final: public Replay
{
public:
    HotReplay(std::shared_ptr<const HotSource> source, HotReplayOptions options = {});

    absl::StatusOr<std::unique_ptr<ReplayStream>> read(StoryId id, Range range) const override;
    absl::StatusOr<std::unique_ptr<ReplayStream>> tail(StoryId id, Event position) const override;

private:
    std::shared_ptr<const HotSource> source_;
    HotReplayOptions options_;
};

} // namespace chronolog::player
