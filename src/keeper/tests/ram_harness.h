#pragma once

#include <memory>

#include "common/clock/FakeClock.h"
#include "keeper/journal/RamJournal.h"

namespace chronolog::test
{

class AssignmentClock final
    : public Clock
    , public CeilingControl
{
public:
    explicit AssignmentClock(int64_t physical)
        : clock_(physical)
    {}
    void setPhysical(int64_t physical) { clock_.setPhysical(physical); }
    void setStatus(ClockStatus status) { clock_.setStatus(status); }
    absl::StatusOr<TimeReading> now() const override { return clock_.now(); }
    absl::StatusOr<std::optional<uint64_t>> uncertainty() const override { return clock_.uncertainty(); }
    Hlc tick() override
    {
        auto value = clock_.tick();
        if(ticked)
            ticked(value);
        return value;
    }
    Hlc observe(Hlc remote) override
    {
        auto value = clock_.observe(remote);
        return value;
    }
    absl::StatusOr<CheckedAssignment> assignChecked(Hlc floor, PhysicalInterval interval) override
    {
        auto value = clock_.assignChecked(floor, interval);
        return value;
    }
    void setCeiling(Hlc ceiling) override { clock_.setCeiling(ceiling); }
    void observeFloor(Hlc floor) override { clock_.observeFloor(floor); }
    int64_t acceptanceClock() override { return clock_.acceptanceClock(); }
    void raiseAcceptanceClock(int64_t floor) override { clock_.raiseAcceptanceClock(floor); }
    std::function<void(Hlc)> ticked;

private:
    FakeClock clock_;
};

// Story 1 at epoch 7, served by the single Keeper "self". Every other story is unknown.
class FakeMembership final: public Membership
{
public:
    void setRoute(Route route)
    {
        std::lock_guard lock(mu_);
        route_ = std::move(route);
    }
    absl::StatusOr<Route> route(StoryId id) const override
    {
        if(id != 1)
            return absl::NotFoundError("unknown story");
        std::lock_guard lock(mu_);
        return route_;
    }
    absl::Status validateEpoch(StoryId id, Epoch epoch) const override
    {
        auto r = route(id);
        if(!r.ok())
            return r.status();
        if(epoch != r->epoch)
            return absl::FailedPreconditionError("stale epoch");
        return absl::OkStatus();
    }
    absl::Status registerProcess(Process) override { return absl::OkStatus(); }
    absl::Status heartbeat(std::string, std::string, uint64_t) override { return absl::OkStatus(); }

private:
    mutable std::mutex mu_;
    Route route_{7, {KeeperRef{"self", "self:1"}}, "grapher:1", "player:1"};
};

class ScannedRamJournal final: public RamJournal
{
public:
    using RamJournal::RamJournal;
    void onSlotValidated(std::function<void()> hook)
    {
        std::lock_guard lock(mu_);
        slot_validated_ = std::move(hook);
    }
    void onAssignment(std::function<void(Hlc)> hook)
    {
        std::lock_guard lock(mu_);
        assigned_ = std::move(hook);
    }
    void onScanned(std::function<void()> hook)
    {
        std::lock_guard lock(mu_);
        scanned_ = std::move(hook);
    }

protected:
    void slotValidated() override
    {
        std::function<void()> hook;
        {
            std::lock_guard lock(mu_);
            hook = slot_validated_;
        }
        if(hook)
            hook();
    }
    void assignmentObserved(Hlc hlc) override
    {
        std::function<void(Hlc)> hook;
        {
            std::lock_guard lock(mu_);
            hook = assigned_;
        }
        if(hook)
            hook(hlc);
    }
    void writerScanned(WriterKey) const override
    {
        std::function<void()> hook;
        {
            std::lock_guard lock(mu_);
            hook = scanned_;
        }
        if(hook)
            hook();
    }

private:
    mutable std::mutex mu_;
    std::function<void()> scanned_;
    std::function<void(Hlc)> assigned_;
    std::function<void()> slot_validated_;
};

// Fresh RamJournal on a FakeClock at physical 100 with writer 2 incarnation 3 registered on story 1.
struct RamRig
{
    std::shared_ptr<AssignmentClock> clock;
    std::shared_ptr<FakeMembership> membership;
    ScannedRamJournal* journal{};

    explicit RamRig(RamJournalConfig config = {})
    {
        config.process_id = "self";
        config.instance = "instance";
        config.append_ceiling_wait_ms = 100;
        clock = std::make_shared<AssignmentClock>(100);
        clock->setStatus(ClockStatus::Synced);
        membership = std::make_shared<FakeMembership>();
        owned_ = std::make_unique<ScannedRamJournal>(clock, membership, config);
        journal = static_cast<ScannedRamJournal*>(owned_.get());
        (void)journal->registerWriter(1, 2, 3);
    }

    std::unique_ptr<RamJournal> release() { return std::move(owned_); }

private:
    std::unique_ptr<RamJournal> owned_;
};

} // namespace chronolog::test
