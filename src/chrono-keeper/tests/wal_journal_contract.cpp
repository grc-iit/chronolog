#include "../../../tests/contract/journal_contract_test.cpp"
#include "wal_harness.h"

namespace chronolog::contract
{
namespace
{

std::unique_ptr<JournalHarness> MakeWal()
{
    auto rig = std::make_shared<test::WalRig>();
    auto h = std::make_unique<JournalHarness>();
    h->payload_limit = rig->ram_config.payload_max_bytes;
    h->supports_durable = true;
    h->setPhysical = [rig](int64_t ns) { rig->clock->setPhysical(ns); };
    h->supersedeIncarnation = [rig] { (void)rig->current->registerWriter(1, 2, 4); };
    h->unassignWriter = [rig] { rig->current->unassignWriter(1, 2); };
    h->releaseIncarnation = [rig] { rig->current->releaseWriter(1, 2, 3); };
    h->registerIdleWriter = [rig](uint64_t writer, uint64_t incarnation)
    { (void)rig->current->registerWriter(1, writer, incarnation); };
    h->crashRestart = [rig, hp = h.get()]
    {
        hp->sut.reset();
        rig->reopen();
        hp->sut = std::move(rig->journal);
    };
    h->blockFsync = [rig] { rig->control->block(); };
    h->waitPendingHlc = [rig] { return rig->control->waitPending(); };
    h->releaseFsync = [rig] { rig->control->release(); };
    h->sut = std::move(rig->journal);
    return h;
}

} // namespace

INSTANTIATE_TEST_SUITE_P(Wal, JournalContract, ::testing::Values(JournalFactory(MakeWal)));

} // namespace chronolog::contract
