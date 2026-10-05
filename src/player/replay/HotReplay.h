#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include "player/replay/HotSource.h"
#include "common/tier/FileTierStore.h"
#include "chronolog/replay.h"

namespace chronolog::player
{

struct HotReplayOptions
{
    size_t batch_size{1024};
    size_t read_max_events{262144};
    size_t tail_max_bytes{64 * 1024 * 1024};
    std::chrono::milliseconds tail_poll{200};
    std::shared_ptr<const FileTierStore> archive;
    // The Catalog's answer for a story. A Tail asks it when a Keeper refused with FAILED_PRECONDITION or the archive
    // records a tombstone, and ends FAILED_PRECONDITION when the answer is FAILED_PRECONDITION. Empty means never.
    std::function<absl::Status(StoryId)> story_live;
};

class ProgressReplayStream: public ReplayStream
{
public:
    virtual std::optional<Hlc> progress() const = 0;
    // R, set with the first progress message of a prefix Tail (I6.15).
    virtual std::optional<uint64_t> revision() const { return std::nullopt; }
};

class HotReplay final: public Replay
{
public:
    HotReplay(std::shared_ptr<const HotSource> source, HotReplayOptions options = {});

    absl::StatusOr<std::unique_ptr<ReplayStream>> read(StoryId id, Range range) const override;
    absl::StatusOr<std::unique_ptr<ReplayStream>> read(StoryId id, Range range, size_t max_events) const;
    absl::StatusOr<std::unique_ptr<ReplayStream>> tail(StoryId id, Event position) const override;
    // I6.17: only matching events are returned or delivered; max_events counts matches and completion is unchanged.
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    read(StoryId id, Range range, const EventPredicate& predicate) const override;
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    read(StoryId id, Range range, size_t max_events, const EventPredicate& predicate) const;
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    tail(StoryId id, Event position, const EventPredicate& predicate) const override;
    // I6.21: a progress Tail also emits an empty batch carrying the frontier when it advances with nothing matching.
    absl::StatusOr<std::unique_ptr<ReplayStream>> tail(StoryId id, Event position, bool progress) const;
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    tail(StoryId id, Event position, const EventPredicate& predicate, bool progress) const;
    // I6.15: one Tail over the sources of every story in a set fixed at Catalog revision R. `position.id.story_id` may
    // be zero. Progress is always on, and the first message names R.
    absl::StatusOr<std::unique_ptr<ReplayStream>>
    tail(std::vector<StoryId> stories, uint64_t revision, Event position, const EventPredicate& predicate) const;

private:
    std::shared_ptr<const HotSource> source_;
    HotReplayOptions options_;
};

} // namespace chronolog::player
