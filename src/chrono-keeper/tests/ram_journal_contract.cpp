#include "../../../tests/contract/journal_contract_test.cpp"
#include "ram_harness.h"

namespace chronolog::contract
{
namespace
{

constexpr int64_t kSkewLimitNs = 1000;

std::unique_ptr<JournalHarness> MakeRam()
{
    RamJournalConfig config;
    config.causal_floor_skew_limit_ns = kSkewLimitNs;
    auto rig = std::make_shared<test::RamRig>(config);

    auto h = std::make_unique<JournalHarness>();
    h->payload_limit = config.payload_max_bytes;
    h->causal_skew_limit_ns = kSkewLimitNs;
    h->supports_durable = false;
    h->setPhysical = [rig](int64_t ns) { rig->clock->setPhysical(ns); };
    h->supersedeIncarnation = [rig] { (void)rig->journal->registerWriter(1, 2, 4); };
    h->unassignWriter = [rig] { rig->journal->unassignWriter(1, 2); };
    h->releaseIncarnation = [rig] { rig->journal->releaseWriter(1, 2, 3); };
    h->registerIdleWriter = [rig](uint64_t writer, uint64_t incarnation)
    { (void)rig->journal->registerWriter(1, writer, incarnation); };
    // A RAM restart is a new Keeper instance: fresh state, same registrations.
    h->crashRestart = [rig, hp = h.get()]
    {
        auto fresh = std::make_shared<test::RamRig>();
        rig->clock = fresh->clock;
        rig->membership = fresh->membership;
        rig->journal = fresh->journal;
        hp->sut = fresh->release();
    };
    h->onAssignment = [rig](std::function<void(Hlc)> hook) { rig->clock->assigned = std::move(hook); };
    h->snapshot = [rig]() -> absl::StatusOr<std::pair<Hlc, std::vector<Event>>>
    {
        auto snapshot = rig->journal->sealedRead(1, All());
        if(!snapshot.ok())
            return snapshot.status();
        return std::pair{snapshot->view.sealed, std::move(snapshot->events)};
    };
    h->sut = rig->release();
    return h;
}

} // namespace

INSTANTIATE_TEST_SUITE_P(Ram,
                         JournalContract,
                         ::testing::Values(JournalFactory(MakeRam)),
                         [](const ::testing::TestParamInfo<JournalFactory>&) { return std::string("Ram"); });

} // namespace chronolog::contract
