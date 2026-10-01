#pragma once

#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "catalog/AcquisitionLedger.h"
#include "chronolog/metadata_store.h"
#include "membership/Topology.h"

namespace chronolog::visor
{

// Test double with the same semantics as SqliteMetadataStore and no persistence.
class InMemoryMetadataStore final
    : public MetadataStore
    , public AcquisitionLedger
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
        KeeperRef assigned_keeper;
    };

    bool hasActiveAcquisition(StoryId id) const;
    // The live identity for a name, else the most recently destroyed one, else npos.
    size_t findChronicle(const std::string& name) const;
    static constexpr size_t kNoChronicle = static_cast<size_t>(-1);
    void notify(const AcquisitionChange& change);

    const Topology topology_;
    const FenceWaiter fence_waiter_;
    mutable std::mutex mutex_;
    // Chronicle identities in creation order. A destroyed identity stays and a reused
    // name appends a new one.
    std::vector<Chronicle> chronicles_;
    std::map<StoryId, Story> stories_;
    // Index into chronicles_ of each story's parent identity.
    std::map<StoryId, size_t> parent_;
    std::map<std::string, uint64_t> writers_;
    std::map<std::pair<StoryId, uint64_t>, AcquisitionRow> acquisitions_;
    // Committed releases by (story, writer, incarnation) with the revision they took.
    std::map<std::tuple<StoryId, uint64_t, uint64_t>, AcquisitionChange> releases_;
    StoryId last_story_id_{};
    uint64_t last_writer_id_{};
    uint64_t revision_{};
    AcquisitionObserver* observer_{};
};

} // namespace chronolog::visor
