#include "catalog/InMemoryMetadataStore.h"

#include <optional>
#include <utility>

namespace chronolog::visor
{

namespace
{
constexpr Epoch kInitialEpoch = 1;
} // namespace

InMemoryMetadataStore::InMemoryMetadataStore(Topology topology, FenceWaiter fence_waiter)
    : topology_(std::move(topology))
    , fence_waiter_(std::move(fence_waiter))
{}

size_t InMemoryMetadataStore::findChronicle(const std::string& name) const
{
    size_t found = kNoChronicle;
    for(size_t i = 0; i < chronicles_.size(); ++i)
    {
        if(chronicles_[i].name != name)
            continue;
        if(!chronicles_[i].tombstoned)
            return i;
        found = i;
    }
    return found;
}

absl::StatusOr<Chronicle> InMemoryMetadataStore::createChronicle(std::string name)
{
    if(!validName(name))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    const size_t existing = findChronicle(name);
    if(existing != kNoChronicle && !chronicles_[existing].tombstoned)
        return absl::AlreadyExistsError("chronicle exists");
    chronicles_.push_back(Chronicle{std::move(name), false});
    return chronicles_.back();
}

absl::StatusOr<Chronicle> InMemoryMetadataStore::getChronicle(std::string name) const
{
    if(!validName(name))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    const size_t index = findChronicle(name);
    if(index == kNoChronicle)
        return absl::NotFoundError("unknown chronicle");
    return chronicles_[index];
}

absl::StatusOr<std::vector<Chronicle>> InMemoryMetadataStore::listChronicles() const
{
    std::lock_guard lock(mutex_);
    return chronicles_;
}

bool InMemoryMetadataStore::hasActiveAcquisition(StoryId id) const
{
    for(auto it = acquisitions_.lower_bound({id, 0}); it != acquisitions_.end() && it->first.first == id; ++it)
        if(!it->second.released)
            return true;
    return false;
}

absl::Status InMemoryMetadataStore::destroyChronicle(std::string name)
{
    if(!validName(name))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    const size_t index = findChronicle(name);
    if(index == kNoChronicle)
        return absl::NotFoundError("unknown chronicle");
    if(chronicles_[index].tombstoned)
        return absl::OkStatus();
    for(const auto& [id, story]: stories_)
        if(parent_.at(id) == index && hasActiveAcquisition(id))
            return absl::FailedPreconditionError("story has an active acquisition");
    chronicles_[index].tombstoned = true;
    for(auto& [id, story]: stories_)
        if(parent_.at(id) == index)
            story.tombstoned = true;
    return absl::OkStatus();
}

absl::StatusOr<Story> InMemoryMetadataStore::createStory(std::string chronicle, std::string name)
{
    if(!validName(chronicle) || !validName(name))
        return absl::InvalidArgumentError("invalid chronicle or story name");
    std::lock_guard lock(mutex_);
    const size_t parent = findChronicle(chronicle);
    if(parent == kNoChronicle)
        return absl::NotFoundError("unknown chronicle");
    if(chronicles_[parent].tombstoned)
        return absl::FailedPreconditionError("chronicle was destroyed");
    for(const auto& [id, story]: stories_)
        if(parent_.at(id) == parent && story.name == name && !story.tombstoned)
            return absl::AlreadyExistsError("story exists");
    Story created{++last_story_id_, std::move(chronicle), std::move(name), kInitialEpoch, false};
    stories_.emplace(created.id, created);
    parent_.emplace(created.id, parent);
    return created;
}

absl::StatusOr<Story> InMemoryMetadataStore::getStory(StoryId id) const
{
    std::lock_guard lock(mutex_);
    auto it = stories_.find(id);
    if(it == stories_.end())
        return absl::NotFoundError("unknown story");
    return it->second;
}

absl::StatusOr<std::vector<Story>> InMemoryMetadataStore::listStories(std::string chronicle) const
{
    if(!validName(chronicle))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    const size_t parent = findChronicle(chronicle);
    if(parent == kNoChronicle)
        return absl::NotFoundError("unknown chronicle");
    std::vector<Story> out;
    for(const auto& [id, story]: stories_)
        if(parent_.at(id) == parent)
            out.push_back(story);
    return out;
}

absl::Status InMemoryMetadataStore::destroyStory(StoryId id)
{
    std::lock_guard lock(mutex_);
    auto it = stories_.find(id);
    if(it == stories_.end())
        return absl::NotFoundError("unknown story");
    if(hasActiveAcquisition(id))
        return absl::FailedPreconditionError("story has an active acquisition");
    it->second.tombstoned = true;
    return absl::OkStatus();
}

absl::StatusOr<Acquisition> InMemoryMetadataStore::acquire(StoryId id, std::string writer_identity)
{
    if(writer_identity.empty())
        return absl::InvalidArgumentError("writer identity is empty");
    AcquisitionChange change;
    Acquisition out;
    {
        std::lock_guard lock(mutex_);
        auto story = stories_.find(id);
        if(story == stories_.end())
            return absl::NotFoundError("unknown story");
        if(story->second.tombstoned)
            return absl::FailedPreconditionError("story was destroyed");
        auto writer = writers_.find(writer_identity);
        if(writer == writers_.end())
            writer = writers_.emplace(std::move(writer_identity), ++last_writer_id_).first;
        const uint64_t writer_id = writer->second;
        auto keeper = topology_.assignKeeper(writer_id, story->second.epoch);
        if(!keeper.ok())
            return keeper.status();
        AcquisitionRow& row = acquisitions_[{id, writer_id}];
        std::optional<AcquisitionChange> superseded;
        // A writer that re-acquires while still active has crashed. Its old
        // incarnation is released together with the new acquire.
        if(row.incarnation != 0 && !row.released)
        {
            superseded = AcquisitionChange{++revision_,
                                           id,
                                           writer_id,
                                           row.incarnation,
                                           row.assigned_keeper,
                                           AcquisitionState::Released};
            releases_[{id, writer_id, row.incarnation}] = *superseded;
        }
        row.incarnation += 1;
        row.released = false;
        row.assigned_keeper = *keeper;
        change = {++revision_, id, writer_id, row.incarnation, row.assigned_keeper, AcquisitionState::Acquired};
        out = {id, writer_id, row.incarnation, topology_.routeFor(story->second.epoch), row.assigned_keeper};
        if(superseded)
            notify(*superseded);
        notify(change);
    }
    return out;
}

absl::StatusOr<ReleaseResult> InMemoryMetadataStore::release(StoryId id, uint64_t writer_id, uint64_t incarnation)
{
    AcquisitionChange change;
    {
        std::lock_guard lock(mutex_);
        auto prior = releases_.find({id, writer_id, incarnation});
        if(prior != releases_.end())
        {
            change = prior->second;
        }
        else
        {
            auto it = acquisitions_.find({id, writer_id});
            if(it == acquisitions_.end() || it->second.incarnation < incarnation)
                return absl::NotFoundError("unknown acquisition");
            if(it->second.incarnation != incarnation || it->second.released)
                return absl::FailedPreconditionError("incarnation is stale");
            it->second.released = true;
            change = {++revision_, id, writer_id, incarnation, it->second.assigned_keeper, AcquisitionState::Released};
            releases_[{id, writer_id, incarnation}] = change;
            notify(change);
        }
    }
    // The wait happens outside the store lock so a slow Keeper never stalls the Catalog.
    const bool fenced = fence_waiter_ && fence_waiter_(change.assigned_keeper, change.revision);
    return ReleaseResult{fenced, change.revision};
}

absl::StatusOr<Epoch> InMemoryMetadataStore::compareAndSetEpoch(StoryId id, Epoch expected, Epoch desired)
{
    if(desired <= expected)
        return absl::InvalidArgumentError("desired epoch must exceed expected");
    std::lock_guard lock(mutex_);
    auto it = stories_.find(id);
    if(it == stories_.end())
        return absl::NotFoundError("unknown story");
    if(it->second.tombstoned)
        return absl::FailedPreconditionError("story was destroyed");
    if(it->second.epoch != expected)
        return absl::FailedPreconditionError("epoch mismatch");
    it->second.epoch = desired;
    return desired;
}

absl::StatusOr<AcquisitionSnapshot> InMemoryMetadataStore::snapshotAcquisitions() const
{
    std::lock_guard lock(mutex_);
    AcquisitionSnapshot snapshot;
    snapshot.revision = revision_;
    for(const auto& [key, row]: acquisitions_)
    {
        if(row.released)
            continue;
        snapshot.active.push_back(
                {revision_, key.first, key.second, row.incarnation, row.assigned_keeper, AcquisitionState::Acquired});
    }
    return snapshot;
}

void InMemoryMetadataStore::setObserver(AcquisitionObserver* observer)
{
    std::lock_guard lock(mutex_);
    observer_ = observer;
}

void InMemoryMetadataStore::notify(const AcquisitionChange& change)
{
    if(observer_)
        observer_->onAcquisitionChange(change);
}

} // namespace chronolog::visor
