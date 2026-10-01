#pragma once

#include <memory>

#include "clock/FakeClock.h"
#include "journal/RamJournal.h"

namespace chronolog::test
{

class AssignmentClock final: public Clock
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
        if(assigned)
            assigned(value);
        return value;
    }
    std::function<void(Hlc)> assigned, ticked;

private:
    FakeClock clock_;
};

// Story 1 at epoch 7, served by the single Keeper "self". Every other story is unknown.
class FakeMembership final: public Membership
{
public:
    absl::StatusOr<Route> route(StoryId id) const override
    {
        if(id != 1)
            return absl::NotFoundError("unknown story");
        return Route{7, {KeeperRef{"self", "self:1"}}, "grapher:1", "player:1"};
    }

    absl::Status validateEpoch(StoryId id, Epoch epoch) const override
    {
        if(id != 1)
            return absl::NotFoundError("unknown story");
        if(epoch != 7)
            return absl::FailedPreconditionError("stale epoch");
        return absl::OkStatus();
    }

    absl::Status registerProcess(Process) override { return absl::OkStatus(); }
    absl::Status heartbeat(std::string, std::string, uint64_t) override { return absl::OkStatus(); }
};

class ScannedRamJournal final: public RamJournal
{
public:
    using RamJournal::RamJournal;
    void onScanned(std::function<void()> hook)
    {
        std::lock_guard lock(mu_);
        scanned_ = std::move(hook);
    }

protected:
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
};

// Fresh RamJournal on a FakeClock at physical 100 with writer 2 incarnation 3 registered on story 1.
struct RamRig
{
    std::shared_ptr<AssignmentClock> clock;
    std::shared_ptr<FakeMembership> membership;
    ScannedRamJournal* journal{};

    explicit RamRig(RamJournalConfig config = {})
    {
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
