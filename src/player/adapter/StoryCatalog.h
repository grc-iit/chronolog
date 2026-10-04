#pragma once

#include <chrono>
#include <memory>
#include <vector>
#include "chronolog/types.h"
#include "chronolog/v1/chronolog.grpc.pb.h"

namespace chronolog::player
{

// Whether a story may be replayed. FAILED_PRECONDITION for a tombstoned or unknown story
// (W10.3, I6.7), UNAVAILABLE when the Catalog cannot be asked.
class StoryCatalog
{
public:
    virtual ~StoryCatalog() = default;
    virtual absl::Status ensureLive(StoryId story) const = 0;
};

// Accepts every story, for runs without a Visor.
class AnyStoryCatalog final: public StoryCatalog
{
public:
    absl::Status ensureLive(StoryId) const override { return absl::OkStatus(); }
};

// The stories under a prefix, read linearizably at Catalog revision `revision` (I9.3).
struct PrefixResolution
{
    uint64_t revision{};
    std::vector<StoryId> stories;
};

// The confirming read of I6.16: `current` is the prefix resolved again, and `created` says that it holds a story
// the earlier resolution did not, so the Read must resolve the set again and repeat.
struct PrefixConfirmation
{
    bool created{};
    PrefixResolution current;
};

class CatalogClient final: public StoryCatalog
{
public:
    CatalogClient(std::shared_ptr<grpc::Channel> visor,
                  std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));
    absl::Status ensureLive(StoryId story) const override;
    absl::StatusOr<bool> tombstoned(StoryId story) const;
    // RESOURCE_EXHAUSTED when more than `limit` stories match.
    absl::StatusOr<PrefixResolution> resolvePrefix(const std::string& prefix, uint32_t limit) const;
    // Run after the last frontier of a prefix Read has been collected (I6.16).
    absl::StatusOr<PrefixConfirmation>
    confirmPrefix(const std::string& prefix, uint32_t limit, const PrefixResolution& resolved) const;

private:
    std::unique_ptr<v1::Catalog::Stub> stub_;
    std::chrono::milliseconds deadline_;
};

} // namespace chronolog::player
