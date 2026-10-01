#include "../../../tests/contract/clock_contract_test.cpp"
#include "clock/FakeClock.h"
#include "clock/SystemClock.h"

namespace chronolog::contract
{
namespace
{

// Delay 20, dispersion 3, drift age 2 give the contract's 15 ns floor.
constexpr uint64_t kBoundNs = 15;

std::unique_ptr<ClockHarness> MakeFake()
{
    auto clock = std::make_unique<FakeClock>(0, kBoundNs);
    auto* raw = clock.get();
    auto h = std::make_unique<ClockHarness>();
    h->stepPhysical = [raw](int64_t ns) { raw->setPhysical(ns); };
    h->setStatus = [raw](ClockStatus status) { raw->setStatus(status); };
    h->sut = std::move(clock);
    return h;
}

struct SourceState
{
    std::atomic<int64_t> physical_ns{0};
    std::atomic<ClockStatus> status{ClockStatus::Unsynced};
};

std::unique_ptr<ClockHarness> MakeSystem()
{
    auto state = std::make_shared<SourceState>();
    SystemClockSource source;
    source.realtime_ns = [state]() -> std::optional<int64_t>
    {
        if(state->status == ClockStatus::Unavailable)
            return std::nullopt;
        return state->physical_ns.load();
    };
    source.status = [state] { return state->status.load(); };
    source.uncertainty_ns = [state]() -> std::optional<uint64_t>
    {
        if(state->status != ClockStatus::Synced)
            return std::nullopt;
        return kBoundNs;
    };
    auto h = std::make_unique<ClockHarness>();
    h->stepPhysical = [state](int64_t ns) { state->physical_ns = ns; };
    h->setStatus = [state](ClockStatus status) { state->status = status; };
    h->sut = std::make_unique<SystemClock>(std::move(source));
    return h;
}

} // namespace

INSTANTIATE_TEST_SUITE_P(Fake, ClockContract, ::testing::Values(ClockFactory(MakeFake)));
INSTANTIATE_TEST_SUITE_P(System, ClockContract, ::testing::Values(ClockFactory(MakeSystem)));

} // namespace chronolog::contract
