#include "catalog/InMemoryMetadataStore.h"

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

absl::StatusOr<Chronicle> InMemoryMetadataStore::createChronicle(std::string name)
{
    if(!validName(name))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    auto it = chronicles_.find(name);
    if(it != chronicles_.end())
    {
        return it->second.tombstoned ? absl::FailedPreconditionError("chronicle was destroyed")
                                     : absl::AlreadyExistsError("chronicle exists");
    }
    Chronicle created{name, false};
    chronicles_.emplace(std::move(name), created);
    return created;
}

absl::StatusOr<Chronicle> InMemoryMetadataStore::getChronicle(std::string name) const
{
    if(!validName(name))
        return absl::InvalidArgumentError("invalid chronicle name");
    std::lock_guard lock(mutex_);
    auto it = chronicles_.find(name);
    if(it == chronicles_.end())
        return absl::NotFoundError("unknown chronicle");
    return it->second;
}

absl::StatusOr<std::vector<Chronicle>> InMemoryMetadataStore::listChronicles() const
{
    std::lock_guard lock(mutex_);
    std::vector<Chronicle> out;
    out.reserve(chronicles_.size());
    for(const auto& [name, chronicle]: chronicles_)
        out.push_back(chronicle);
    return out;
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
    auto it = chronicles_.find(name);
    if(it == chronicles_.end())
        return absl::NotFoundError("unknown chronicle");
    for(const auto& [id, story]: stories_)
        if(story.chronicle == name && hasActiveAcquisition(id))
            return absl::FailedPreconditionError("story has an active acquisition");
    it->second.tombstoned = true;
    for(auto& [id, story]: stories_)
        if(story.chronicle == name)
            story.tombstoned = true;
    return absl::OkStatus();
}

absl::StatusOr<Story> InMemoryMetadataStore::createStory(std::string chronicle, std::string name)
{
    if(!validName(chronicle) || !validName(name))
        return absl::InvalidArgumentError("invalid chronicle or story name");
    std::lock_guard lock(mutex_);
    auto parent = chronicles_.find(chronicle);
    if(parent == chronicles_.end())
        return absl::NotFoundError("unknown chronicle");
    if(parent->second.tombstoned)
        return absl::FailedPreconditionError("chronicle was destroyed");
    for(const auto& [id, story]: stories_)
    {
        if(story.chronicle == chronicle && story.name == name)
        {
            return story.tombstoned ? absl::FailedPreconditionError("story was destroyed")
                                    : absl::AlreadyExistsError("story exists");
        }
    }
    Story created{++last_story_id_, std::move(chronicle), std::move(name), kInitialEpoch, false};
    stories_.emplace(created.id, created);
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
    if(chronicles_.find(chronicle) == chronicles_.end())
        return absl::NotFoundError("unknown chronicle");
    std::vector<Story> out;
    for(const auto& [id, story]: stories_)
        if(story.chronicle == chronicle)
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
        // A writer that re-acquires while still active supersedes its previous
        // incarnation, which is how a crashed writer recovers.
        row.incarnation += 1;
        row.released = false;
        row.assigned_keeper = *keeper;
        change = {++revision_, id, writer_id, row.incarnation, row.assigned_keeper, AcquisitionState::Acquired};
        out = {id, writer_id, row.incarnation, topology_.routeFor(story->second.epoch), row.assigned_keeper};
        notify(change);
    }
    return out;
}

absl::StatusOr<ReleaseResult> InMemoryMetadataStore::release(StoryId id, uint64_t writer_id, uint64_t incarnation)
{
    AcquisitionChange change;
    {
        std::lock_guard lock(mutex_);
        auto it = acquisitions_.find({id, writer_id});
        if(it == acquisitions_.end())
            return absl::NotFoundError("unknown acquisition");
        if(it->second.released || it->second.incarnation != incarnation)
            return absl::FailedPreconditionError("incarnation is stale or already released");
        it->second.released = true;
        change = {++revision_, id, writer_id, incarnation, it->second.assigned_keeper, AcquisitionState::Released};
        notify(change);
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
