#pragma once

#include <chrono>
#include <memory>
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

class CatalogClient final: public StoryCatalog
{
public:
    CatalogClient(std::shared_ptr<grpc::Channel> visor,
                  std::chrono::milliseconds deadline = std::chrono::milliseconds(2000));
    absl::Status ensureLive(StoryId story) const override;
    absl::StatusOr<bool> tombstoned(StoryId story) const;

private:
    std::unique_ptr<v1::Catalog::Stub> stub_;
    std::chrono::milliseconds deadline_;
};

} // namespace chronolog::player
