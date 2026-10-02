#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include "chrono-player/replay/HotSource.h"
#include "chrono-grapher/tier/FileTierStore.h"
#include "chronolog/replay.h"

namespace chronolog::player
{

struct HotReplayOptions
{
    size_t batch_size{1024};
    size_t read_max_events{262144};
    std::chrono::milliseconds tail_poll{200};
    std::shared_ptr<const FileTierStore> archive;
    // The Catalog's answer for a story. A Tail asks it when a Keeper refused with FAILED_PRECONDITION or the archive
    // records a tombstone, and ends FAILED_PRECONDITION when the answer is FAILED_PRECONDITION. Empty means never.
    std::function<absl::Status(StoryId)> story_live;
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
