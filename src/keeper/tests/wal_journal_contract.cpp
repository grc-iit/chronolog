#include "keeper/membership/AcquisitionWatcher.h"
#include "journal_contract_test.cpp"
#include "journal_capacity_contract_test.inc"
#include "keeper/tests/wal_harness.h"
#include "keeper/archive/KeeperArchive.h"
#include "keeper/membership/ConfigMembership.h"

namespace chronolog::contract
{
namespace
{

std::unique_ptr<JournalHarness> MakeWal(uint32_t window_us)
{
    auto rig = std::make_shared<test::WalRig>(4096, 16, window_us);
    auto h = std::make_unique<JournalHarness>();
    h->payload_limit = rig->ram_config.payload_max_bytes;
    h->supports_durable = true;
    h->setPhysical = [rig](int64_t ns) { rig->clock->setPhysical(ns); };
    h->supersedeIncarnation = [rig] { (void)rig->current->registerWriter(1, 2, 4); };
    h->unassignWriter = [rig] { rig->current->unassignWriter(1, 2); };
    h->releaseIncarnation = [rig] { rig->current->releaseWriter(1, 2, 3); };
    h->registerIdleWriter = [rig](uint64_t writer, uint64_t incarnation)
    { (void)rig->current->registerWriter(1, writer, incarnation); };
    auto archive = std::make_shared<std::unique_ptr<keeper::KeeperArchive>>();
    auto membership = std::make_shared<keeper::ConfigMembership>(
            std::vector<keeper::StaticRoute>{{1, {7, {{"self", "self:1"}}, "127.0.0.1:1", ""}}});
    h->sealArchive = [rig, archive, membership]() -> absl::StatusOr<std::vector<Chunk>>
    {
        if(!*archive)
        {
            keeper::KeeperArchiveConfig config;
            config.story_chunk_duration_secs = 1;
            config.archive_visibility_delay_secs = 0;
            *archive = std::make_unique<keeper::KeeperArchive>(*rig->current, *membership, "keeper", config);
        }
        auto status = (*archive)->seal();
        if(!status.ok())
            return status;
        return (*archive)->chunks();
    };
    h->deliverChunk = [archive](const Chunk& chunk, std::string instance, uint64_t number)
    {
        internal::v1::ChunkReceipt receipt;
        receipt.set_chunk_id(chunk.id);
        receipt.set_bytes(123);
        receipt.set_grapher_instance(std::move(instance));
        receipt.set_receipt(number);
        (*archive)->delivered(chunk.id, receipt, 123);
    };
    h->reportArchive = [archive](WatermarkReport report) { (*archive)->applyReport(report); };
    h->releaseTail = [archive] { (*archive)->releaseTail(1); };
    h->evictionFloor = [rig] { return rig->current->evictionFloor(1); };
    h->walSegments = [rig]
    {
        std::set<std::string> paths;
        for(const auto& entry: std::filesystem::directory_iterator(rig->control->directory))
            if(entry.path().extension() == ".wal")
                paths.insert(entry.path().filename().string());
        return paths;
    };
    h->applySupersession = [rig](bool snapshot)
    {
        keeper::AcquisitionWatcher watcher(*rig->current, "self", [] {}, false);
        internal::v1::AcquisitionUpdate update;
        update.set_story_id(1);
        update.set_writer_id(2);
        update.set_incarnation(4);
        update.set_revision(2);
        update.set_state(internal::v1::ACQUISITION_STATE_ACQUIRED);
        update.mutable_assigned_keeper()->set_process_id("self");
        if(snapshot)
        {
            internal::v1::AcquisitionSnapshot state;
            state.set_revision(2);
            *state.add_acquisitions() = update;
            EXPECT_TRUE(watcher.applySnapshot(state));
        }
        else
        {
            auto release = update;
            release.set_incarnation(3);
            release.set_revision(1);
            release.set_state(internal::v1::ACQUISITION_STATE_RELEASED);
            watcher.applyUpdate(release);
            watcher.applyUpdate(update);
        }
    };
    h->drainAdmissionEvidence = [rig] { return rig->current->drainAdmissionEvidence().size(); };
    h->terminateIncarnation = [rig](AcquisitionTerminationCause cause) { rig->current->releaseWriter(1, 2, 3, cause); };
    h->rejection_reasons = true;
    h->dedupe_window = rig->ram_config.dedupe_window;
    h->onSlotValidated = [rig](std::function<void()> hook) { rig->current->onSlotValidated(std::move(hook)); };
    h->onAssignment = [rig](std::function<void(Hlc)> hook) { rig->current->onAssignment(std::move(hook)); };
    h->onWriterScanned = [rig](std::function<void()> hook) { rig->current->scanned = std::move(hook); };
    h->snapshot = [rig]() -> absl::StatusOr<std::pair<Hlc, std::vector<Event>>>
    {
        auto snapshot = rig->current->sealedRead(1, All());
        if(!snapshot.ok())
            return snapshot.status();
        return std::pair{snapshot->view.sealed, std::move(snapshot->events)};
    };
    h->failFsync = [rig](bool fail)
    {
        std::lock_guard lock(rig->control->mu);
        rig->control->fail = fail;
    };
    h->crashRestart = [rig, archive, hp = h.get()]
    {
        archive->reset();
        hp->sut.reset();
        rig->reopen();
        hp->sut = std::move(rig->journal);
    };
    h->blockFsync = [rig] { rig->control->block(); };
    h->waitPendingHlc = [rig] { return rig->control->waitPending(); };
    h->releaseFsync = [rig] { rig->control->release(); };
    h->enableDynamic = [rig](Hlc c, int64_t cp)
    {
        rig->current->enableDynamic("instance");
        rig->current->extendCeiling(c, cp);
        RouteState state;
        state.route = *rig->membership->route(1);
        rig->current->applyRoute(1, state, false, 1, [] {});
    };
    h->enableDynamicBeforeRoute = [rig](Hlc c, int64_t cp)
    {
        rig->current->enableDynamic("instance");
        rig->current->extendCeiling(c, cp);
    };
    h->ceilingWaiting = [rig] { return rig->current->ceilingWaiters() != 0; };
    h->extendCeiling = [rig](Hlc c, int64_t cp) { rig->current->extendCeiling(c, cp); };
    h->applyRoute = [rig](RouteState state, bool observe, uint64_t revision)
    { rig->current->applyRoute(1, state, observe, revision, [&] { rig->membership->setRoute(state.route); }); };
    h->acceptanceClock = [rig] { return rig->clock->acceptanceClock(); };
    h->retiredDrained = [rig] { return rig->current->retiredDrained(1); };
    h->tombstone = [rig] { EXPECT_TRUE(rig->current->dropStory(1, true).ok()); };
    h->forceCapacity = [rig](bool reached) { rig->current->setAdmissionCapacityReached(reached); };
    h->sut = std::move(rig->journal);
    return h;
}

} // namespace

INSTANTIATE_TEST_SUITE_P(Wal, JournalContract, ::testing::Values(JournalFactory([] { return MakeWal(0); })));
// I5.6 and every other Journal contract hold with a group-commit window too.
INSTANTIATE_TEST_SUITE_P(WalWindow, JournalContract, ::testing::Values(JournalFactory([] { return MakeWal(1000); })));

} // namespace chronolog::contract
