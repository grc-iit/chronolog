// An identity that acquires while it still holds an active acquisition has crashed:
// the old incarnation is released and the next one created in one step.
#include <gtest/gtest.h>

#include <mutex>
#include <vector>

#include "TestSupport.h"
#include "catalog/InMemoryMetadataStore.h"
#include "catalog/SqliteMetadataStore.h"

namespace chronolog::visor
{
namespace
{

using testing::TempDir;
using testing::twoKeeperTopology;

class Recorder final: public AcquisitionObserver
{
public:
    void onAcquisitionChange(const AcquisitionChange& change) override
    {
        std::lock_guard lock(mutex_);
        changes.push_back(change);
    }
    std::vector<AcquisitionChange> changes;

private:
    std::mutex mutex_;
};

template <class Store>
void checkSupersede(Store& store, Recorder& recorder)
{
    store.setObserver(&recorder);
    ASSERT_TRUE(store.createChronicle("c").ok());
    const StoryId story = store.createStory("c", "s")->id;

    auto first = store.acquire(story, "writer");
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first->incarnation, 1u);
    auto second = store.acquire(story, "writer");
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second->writer_id, first->writer_id);
    EXPECT_EQ(second->incarnation, 2u);
    EXPECT_EQ(second->assigned_keeper, first->assigned_keeper);

    // Acquired(1), Released(1), Acquired(2), each with its own increasing revision, and
    // the release goes to the Keeper that held the old incarnation.
    ASSERT_EQ(recorder.changes.size(), 3u);
    EXPECT_EQ(recorder.changes[0].state, AcquisitionState::Acquired);
    EXPECT_EQ(recorder.changes[1].state, AcquisitionState::Released);
    EXPECT_EQ(recorder.changes[1].incarnation, 1u);
    EXPECT_EQ(recorder.changes[1].assigned_keeper, first->assigned_keeper);
    EXPECT_EQ(recorder.changes[2].state, AcquisitionState::Acquired);
    EXPECT_EQ(recorder.changes[2].incarnation, 2u);
    EXPECT_LT(recorder.changes[0].revision, recorder.changes[1].revision);
    EXPECT_LT(recorder.changes[1].revision, recorder.changes[2].revision);

    // Only the new incarnation is active, and the old one's release is idempotent
    // at the revision the supersede committed.
    auto snapshot = store.snapshotAcquisitions();
    ASSERT_TRUE(snapshot.ok());
    ASSERT_EQ(snapshot->active.size(), 1u);
    EXPECT_EQ(snapshot->active[0].incarnation, 2u);
    auto old_release = store.release(story, first->writer_id, 1);
    ASSERT_TRUE(old_release.ok());
    EXPECT_EQ(old_release->revision, recorder.changes[1].revision);
    EXPECT_EQ(recorder.changes.size(), 3u);
    EXPECT_EQ(store.destroyStory(story).code(), absl::StatusCode::kFailedPrecondition);
    ASSERT_TRUE(store.release(story, second->writer_id, 2).ok());
    EXPECT_TRUE(store.destroyStory(story).ok());
    store.setObserver(nullptr);
}

TEST(supersede, AcquireWhileActiveSupersedesOldIncarnationInMemory)
{
    InMemoryMetadataStore store(twoKeeperTopology());
    Recorder recorder;
    checkSupersede(store, recorder);
}

TEST(supersede, AcquireWhileActiveSupersedesOldIncarnationSqlite)
{
    TempDir dir;
    auto store = SqliteMetadataStore::open((dir.path() / "catalog.sqlite").string(), twoKeeperTopology());
    ASSERT_TRUE(store.ok()) << store.status();
    Recorder recorder;
    checkSupersede(**store, recorder);
}

TEST(supersede, SupersededIncarnationSurvivesReopen)
{
    TempDir dir;
    const std::string path = (dir.path() / "catalog.sqlite").string();
    StoryId story = 0;
    uint64_t writer_id = 0;
    uint64_t supersede_revision = 0;
    {
        auto store = SqliteMetadataStore::open(path, twoKeeperTopology());
        ASSERT_TRUE(store.ok());
        Recorder recorder;
        (*store)->setObserver(&recorder);
        ASSERT_TRUE((*store)->createChronicle("c").ok());
        story = (*store)->createStory("c", "s")->id;
        writer_id = (*store)->acquire(story, "w")->writer_id;
        ASSERT_TRUE((*store)->acquire(story, "w").ok());
        supersede_revision = recorder.changes.at(1).revision;
        (*store)->setObserver(nullptr);
    }
    auto reopened = SqliteMetadataStore::open(path, twoKeeperTopology());
    ASSERT_TRUE(reopened.ok());
    auto retry = (*reopened)->release(story, writer_id, 1);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry->revision, supersede_revision);
    auto third = (*reopened)->acquire(story, "w");
    ASSERT_TRUE(third.ok());
    EXPECT_EQ(third->incarnation, 3u);
}

} // namespace
} // namespace chronolog::visor
