#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "catalog/AcquisitionLedger.h"
#include "chronolog/metadata_store.h"
#include "membership/Topology.h"

namespace chronolog::visor
{

// Test double with the same semantics as SqliteMetadataStore and no persistence.
class InMemoryMetadataStore final: public MetadataStore, public AcquisitionLedger
{
public:
    // `fence_waiter` may be empty, in which case release always reports fenced=false.
    explicit InMemoryMetadataStore(Topology topology, FenceWaiter fence_waiter = nullptr);

    absl::StatusOr<Chronicle> createChronicle(std::string name) override;
    absl::StatusOr<Chronicle> getChronicle(std::string name) const override;
    absl::StatusOr<std::vector<Chronicle>> listChronicles() const override;
    absl::Status destroyChronicle(std::string name) override;
    absl::StatusOr<Story> createStory(std::string chronicle, std::string name) override;
    absl::StatusOr<Story> getStory(StoryId id) const override;
    absl::StatusOr<std::vector<Story>> listStories(std::string chronicle) const override;
    absl::Status destroyStory(StoryId id) override;
    absl::StatusOr<Acquisition> acquire(StoryId id, std::string writer_identity) override;
    absl::StatusOr<ReleaseResult> release(StoryId id, uint64_t writer_id, uint64_t incarnation) override;
    absl::StatusOr<Epoch> compareAndSetEpoch(StoryId id, Epoch expected, Epoch desired) override;

    absl::StatusOr<AcquisitionSnapshot> snapshotAcquisitions() const override;
    void setObserver(AcquisitionObserver* observer) override;

private:
    struct AcquisitionRow
    {
        uint64_t incarnation{};
        bool released{};
        std::string assigned_keeper;
    };

    bool hasActiveAcquisition(StoryId id) const;
    void notify(const AcquisitionChange& change);

    const Topology topology_;
    const FenceWaiter fence_waiter_;
    mutable std::mutex mutex_;
    std::map<std::string, Chronicle> chronicles_;
    std::map<StoryId, Story> stories_;
    std::map<std::string, uint64_t> writers_;
    std::map<std::pair<StoryId, uint64_t>, AcquisitionRow> acquisitions_;
    StoryId last_story_id_{};
    uint64_t last_writer_id_{};
    uint64_t revision_{};
    AcquisitionObserver* observer_{};
};

} // namespace chronolog::visor
